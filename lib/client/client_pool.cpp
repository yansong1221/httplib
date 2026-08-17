#include "httplib/client/client_pool.hpp"
#include "httplib/client/client.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url.hpp>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace httplib::client
{

    namespace
    {

        struct pooled_conn
        {
            std::unique_ptr<http_client> client;
            std::chrono::steady_clock::time_point idle_since;
        };

        struct pool_state
        {
            std::vector<pooled_conn> idle;
            int64_t active_count = 0;
        };

        struct waiter_node
        {
            net::steady_timer timer;
            std::string url;

            waiter_node(net::any_io_executor const& ex, std::string u) : timer(ex), url(std::move(u)) {}
        };

    } // namespace

    class http_client_pool::impl : public std::enable_shared_from_this<impl>
    {
      public:
        impl(net::any_io_executor const& ex, size_t max_size, std::chrono::steady_clock::duration idle_timeout)
            : ex_(ex)
            , max_size_(max_size)
            , idle_timeout_(idle_timeout)
        {
        }

        ~impl() { stop(); }

        void
        start()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopped_.exchange(false))
            {
                return;
            }

            // Each maintenance loop owns a fresh timer. stop() cancels the timer it
            // finds here, so a stop()/start() sequence can never make two co_maintain
            // coroutines share a single steady_timer (which would be UB).
            cleanup_timer_ = std::make_shared<net::steady_timer>(ex_);
            auto timer = cleanup_timer_;

            net::co_spawn(
                ex_,
                [this, self = shared_from_this(), timer]() -> net::awaitable<void>
                { co_await co_maintain(timer); },
                [](std::exception_ptr e)
                {
                    if (e)
                    {
                        std::rethrow_exception(e);
                    }
                });
        }

        net::awaitable<client_handle>
        async_acquire(std::string_view host, uint16_t port, bool ssl, std::chrono::steady_clock::duration wait_timeout)
        {
            if (stopped_)
            {
                co_return client_handle(boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
            }

            auto self = shared_from_this();
            auto url = util::make_url_value(host, port, ssl);

            // wait_timeout <= 0 means "fail fast": try once and return timed_out
            // immediately if no connection is available without waiting. The deadline
            // is only consulted on the waiting path (wait_timeout > 0).
            auto deadline = std::chrono::steady_clock::now() + wait_timeout;

            do
            {
                std::unique_lock<std::mutex> lock(self->mutex_);
                if (self->stopped_)
                {
                    co_return client_handle(
                        boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
                }

                if (auto handle = self->acquire_or_create(url); handle)
                {
                    co_return std::move(handle);
                }

                if (wait_timeout <= std::chrono::steady_clock::duration::zero())
                {
                    co_return client_handle(boost::system::errc::make_error_code(boost::system::errc::timed_out));
                }

                std::erase_if(self->waiters_, [](auto const& w) { return w.expired(); });

                auto node = std::make_shared<waiter_node>(self->ex_, url);
                node->timer.expires_at(deadline);

                self->waiters_.push_back(node);
                lock.unlock();

                boost::system::error_code ec;
                co_await node->timer.async_wait(util::net_awaitable[ec]);

            } while (deadline > std::chrono::steady_clock::now());

            co_return client_handle(boost::system::errc::make_error_code(boost::system::errc::timed_out));
        }

        void
        release(std::unique_ptr<http_client> conn)
        {
            auto url = util::make_url_value(conn->host(), conn->port(), conn->is_use_ssl());

            std::lock_guard<std::mutex> lock(mutex_);

            if (stopped_)
            {
                return;
            }

            auto st_it = pools_.find(url);
            if (st_it == pools_.end())
            {
                return;
            }
            st_it->second.active_count--;

            if (!conn->has_active_session() && st_it->second.active_count < static_cast<int64_t>(max_size_))
            {
                // conn->close();
                // FIXME: close disabled for testing

                st_it->second.idle.push_back({ std::move(conn), std::chrono::steady_clock::now() });
            }

            for (auto it = waiters_.begin(); it != waiters_.end();)
            {
                if (it->expired())
                {
                    it = waiters_.erase(it);
                    continue;
                }
                if (auto waiter = it->lock(); waiter && waiter->url == url)
                {
                    waiter->timer.cancel();
                    waiters_.erase(it);
                    break;
                }
                ++it;
            }
        }

        void
        stop()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopped_.exchange(true))
            {
                return;
            }
            if (cleanup_timer_)
            {
                cleanup_timer_->cancel();
                cleanup_timer_.reset();
            }

            waiters_list waiters;
            waiters.swap(waiters_);

            for (auto& [info, st] : pools_)
            {
                for (auto& pc : st.idle)
                {
                    pc.client->close();
                }
                st.idle.clear();
            }
            pools_.clear();

            lock.unlock();

            while (!waiters.empty())
            {
                auto w = waiters.front();
                waiters.pop_front();
                if (auto waiter = w.lock(); waiter)
                {
                    waiter->timer.cancel();
                }
            }
        }

        pool_stats
        stats(std::string const& url) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pool_stats s;
            auto it = pools_.find(url);
            if (it != pools_.end())
            {
                s.idle = it->second.idle.size();
                s.active = it->second.active_count;
            }
            return s;
        }

        void
        set_max_size(size_t n)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            max_size_ = n;
            for (auto& [info, st] : pools_)
            {
                while (st.idle.size() > max_size_)
                {
                    st.idle.front().client->close();
                    st.idle.erase(st.idle.begin());
                }
            }
        }

        void
        set_idle_timeout(std::chrono::steady_clock::duration timeout)
        {
            idle_timeout_.store(timeout);
        }

      private:
        client_handle
        acquire_or_create(std::string const& url)
        {
            auto [it, inserted] = pools_.try_emplace(url);
            auto& st = it->second;

            if (!inserted && !st.idle.empty())
            {
                auto conn = std::move(st.idle.front().client);
                st.idle.erase(st.idle.begin());
                st.active_count++;
                return client_handle(shared_from_this(), std::move(conn));
            }

            if (st.active_count + st.idle.size() < max_size_)
            {
                st.active_count++;
                return client_handle(shared_from_this(), std::make_unique<http_client>(ex_, url));
            }

            return {};
        }

        net::awaitable<void>
        co_maintain(std::shared_ptr<net::steady_timer> timer)
        {
            boost::system::error_code ec;
            while (!stopped_)
            {
                auto timeout = idle_timeout_.load();
                auto interval = timeout > std::chrono::seconds(0) ? timeout : std::chrono::seconds(60);

                timer->expires_after(interval);
                co_await timer->async_wait(util::net_awaitable[ec]);
                if (ec || stopped_)
                {
                    co_return;
                }

                auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto it = pools_.begin(); it != pools_.end();)
                {
                    auto& st = it->second;
                    auto it2 = st.idle.begin();
                    while (it2 != st.idle.end())
                    {
                        if (now - it2->idle_since > idle_timeout_.load())
                        {
                            it2->client->close();
                            it2 = st.idle.erase(it2);
                        }
                        else
                        {
                            ++it2;
                        }
                    }
                    if (st.idle.empty() && st.active_count == 0)
                    {
                        it = pools_.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
        }

      private:
        using waiters_list = std::deque<std::weak_ptr<waiter_node>>;

        mutable std::mutex mutex_;
        std::unordered_map<std::string, pool_state> pools_;
        size_t max_size_;
        std::atomic<std::chrono::steady_clock::duration> idle_timeout_;

        waiters_list waiters_;

      public:
        net::any_io_executor ex_;

      private:
        std::atomic<bool> stopped_ { true };
        std::shared_ptr<net::steady_timer> cleanup_timer_;
    };

    // ---- client_handle ----

    http_client_pool::client_handle::client_handle()
        : client_handle(boost::system::errc::make_error_code(boost::system::errc::not_connected))
    {
    }

    http_client_pool::client_handle::client_handle(std::weak_ptr<impl> pool, std::unique_ptr<http_client> conn)
        : pool_(std::move(pool))
        , conn_(std::move(conn))
    {
    }

    http_client_pool::client_handle::client_handle(boost::system::error_code ec) : error_(ec) {}

    http_client_pool::client_handle::client_handle(client_handle&& other) noexcept
        : pool_(std::move(other.pool_))
        , conn_(std::move(other.conn_))
        , error_(other.error_)
    {
    }

    http_client_pool::client_handle&
    http_client_pool::client_handle::operator=(client_handle&& other) noexcept
    {
        if (this != &other)
        {
            release();
            pool_ = std::move(other.pool_);
            conn_ = std::move(other.conn_);
            error_ = other.error_;
        }
        return *this;
    }

    http_client_pool::client_handle::~client_handle() { release(); }

    void
    http_client_pool::client_handle::release()
    {
        auto pool = pool_.lock();
        if (pool && conn_)
        {
            pool->release(std::move(conn_));
        }
    }

    http_client*
    http_client_pool::client_handle::get()
    {
        if (has_error())
        {
            throw boost::system::system_error(error_);
        }
        return conn_.get();
    }

    http_client const*
    http_client_pool::client_handle::get() const
    {
        if (has_error())
        {
            throw boost::system::system_error(error_);
        }
        return conn_.get();
    }

    http_client_pool::client_handle::operator bool() const noexcept { return !has_error(); }

    bool
    http_client_pool::client_handle::has_error() const noexcept
    {
        return conn_ == nullptr;
    }

    http_client*
    http_client_pool::client_handle::operator->()
    {
        return get();
    }

    http_client const*
    http_client_pool::client_handle::operator->() const
    {
        return get();
    }

    http_client&
    http_client_pool::client_handle::operator*()
    {
        return *get();
    }

    http_client const&
    http_client_pool::client_handle::operator*() const
    {
        return *get();
    }

    boost::system::error_code const&
    http_client_pool::client_handle::error() const noexcept
    {
        return error_;
    }

    // ---- connection_pool ----

    http_client_pool::http_client_pool(net::any_io_executor const& ex,
                                       size_t max_size,
                                       std::chrono::steady_clock::duration idle_timeout)
        : impl_(std::make_shared<impl>(ex, max_size, idle_timeout))
    {
    }

    http_client_pool::~http_client_pool()
    {
        if (impl_)
        {
            impl_->stop();
        }
    }

    http_client_pool::http_client_pool(http_client_pool&& other) noexcept = default;

    http_client_pool&
    http_client_pool::operator=(http_client_pool&& other) noexcept = default;

    void
    http_client_pool::start()
    {
        impl_->start();
    }

    net::awaitable<http_client_pool::client_handle>
    http_client_pool::async_acquire(
        std::string_view host,
        uint16_t port,
        bool ssl /*= false*/,
        std::chrono::steady_clock::duration wait_timeout /*= default_timeout*/)
    {
        co_return co_await impl_->async_acquire(host, port, ssl, wait_timeout);
    }

    net::awaitable<http_client_pool::client_handle>
    http_client_pool::async_acquire(
        std::string_view url,
        std::chrono::steady_clock::duration wait_timeout /*= default_timeout*/)
    {
        auto r = boost::urls::parse_uri(url);
        if (!r)
        {
            co_return client_handle(r.error());
        }
        auto const& u = *r;
        auto host = u.host();
        auto port = u.port_number() ? u.port_number() : (u.scheme_id() == boost::urls::scheme::https ? 443 : 80);
        auto ssl = u.scheme_id() == boost::urls::scheme::https;
        co_return co_await impl_->async_acquire(host, port, ssl, wait_timeout);
    }

    net::any_io_executor
    http_client_pool::get_executor() noexcept
    {
        return impl_->ex_;
    }

    void
    http_client_pool::set_max_size(size_t n)
    {
        impl_->set_max_size(n);
    }

    void
    http_client_pool::set_idle_timeout(std::chrono::steady_clock::duration timeout)
    {
        impl_->set_idle_timeout(timeout);
    }

    void
    http_client_pool::stop()
    {
        impl_->stop();
    }

    http_client_pool::pool_stats
    http_client_pool::stats(std::string_view host, uint16_t port, bool ssl /*= false*/) const
    {
        auto url = util::make_url_value(host, port, ssl);
        return impl_->stats(url);
    }

    httplib::client::http_client_pool::pool_stats
    http_client_pool::stats(std::string_view url) const
    {
        auto r = boost::urls::parse_uri(url);
        if (!r)
        {
            return {};
        }
        auto const& u = *r;
        auto host = u.host();
        auto port = u.port_number() ? u.port_number() : (u.scheme_id() == boost::urls::scheme::https ? 443 : 80);
        auto ssl = u.scheme_id() == boost::urls::scheme::https;
        return impl_->stats(util::make_url_value(host, port, ssl));
    }

} // namespace httplib::client
