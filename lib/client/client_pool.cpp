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
#include "util/logging.hpp"
#include <spdlog/spdlog.h>
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
            std::deque<pooled_conn> idle;
            int64_t active_count = 0;
        };

        struct waiter_node
        {
            net::steady_timer timer;

            explicit waiter_node(net::any_io_executor const& ex) : timer(ex) {}
        };

    } // namespace

    class http_client_pool::impl : public std::enable_shared_from_this<impl>
    {
      public:
        impl(net::any_io_executor const& ex, pool_params cfg)
            : ex_(ex)
            , cfg_(std::move(cfg))
        {
            default_logger_ = httplib::detail::make_console_logger("httplib.client_pool");
        }

        ~impl() { stop(); }

        std::shared_ptr<spdlog::logger>
        logger() const
        {
            return custom_logger_ ? custom_logger_ : default_logger_;
        }

        void
        set_logger(std::shared_ptr<spdlog::logger> l)
        {
            custom_logger_ = std::move(l);
        }

        void
        start()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopped_.exchange(false))
            {
                return;
            }
            logger()->debug("client pool started");

            // Each maintenance loop owns a fresh timer. stop() cancels the timer it
            // finds here, so a stop()/start() sequence can never make two co_maintain
            // coroutines share a single steady_timer (which would be UB).
            cleanup_timer_ = std::make_shared<net::steady_timer>(ex_);
            auto timer = cleanup_timer_;

            net::co_spawn(
                ex_,
                [this, self = shared_from_this(), timer]() -> net::awaitable<void>
                { co_await co_maintain(timer); },
                [self = shared_from_this()](std::exception_ptr e)
                {
                    if (e)
                    {
                        try
                        {
                            std::rethrow_exception(e);
                        }
                        catch (std::exception const& ex)
                        {
                            self->logger()->error("client pool maintenance failed: {}", ex.what());
                        }
                        catch (...)
                        {
                            self->logger()->error("client pool maintenance failed: unknown error");
                        }
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
                    logger()->debug("client pool: no available connection for {} (fail fast)", url);
                    co_return client_handle(boost::system::errc::make_error_code(boost::system::errc::timed_out));
                }

                auto& waiters = self->waiters_[url];
                std::erase_if(waiters, [](auto const& w) { return w.expired(); });

                auto node = std::make_shared<waiter_node>(self->ex_);
                node->timer.expires_at(deadline);

                waiters.push_back(node);
                lock.unlock();

                boost::system::error_code ec;
                co_await node->timer.async_wait(util::net_awaitable[ec]);

            } while (deadline > std::chrono::steady_clock::now());

            logger()->debug("client pool: acquire timed out for {}", url);
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
            --total_active_;

            if (!conn->has_active_session() && st_it->second.active_count < static_cast<int64_t>(cfg_.max_size))
            {
                // No streaming session still uses the connection: keep it open and
                // return it to the idle pool for reuse (keep-alive).
                st_it->second.idle.push_back({ std::move(conn), std::chrono::steady_clock::now() });
                logger()->trace("client pool: returned connection to idle for {}", url);
            }
            else
            {
                track_destroyed();
                logger()->trace("client pool: closed connection for {}", url);
            }

            wake_one_waiter(url);
        }

        void
        stop()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopped_.exchange(true))
            {
                return;
            }
            logger()->debug("client pool stopped");
            if (cleanup_timer_)
            {
                cleanup_timer_->cancel();
                cleanup_timer_.reset();
            }

            std::vector<waiters_list> pending_waiters;
            pending_waiters.reserve(waiters_.size());
            for (auto& [url, waiters] : waiters_)
            {
                pending_waiters.push_back(std::move(waiters));
            }
            waiters_.clear();

            for (auto& [info, st] : pools_)
            {
                for (auto& pc : st.idle)
                {
                    pc.client->close();
                }
                st.idle.clear();
            }
            pools_.clear();
            total_connections_ = 0;
            total_active_ = 0;

            lock.unlock();

            for (auto& waiters : pending_waiters)
            {
                while (!waiters.empty())
                {
                    auto w = std::move(waiters.front());
                    waiters.pop_front();
                    if (auto waiter = w.lock(); waiter)
                    {
                        waiter->timer.cancel();
                    }
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

        size_t
        active_count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return total_active_;
        }

        size_t
        idle_count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return total_connections_ - total_active_;
        }

        size_t
        total_count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return total_connections_;
        }

      private:
        void
        track_created()
        {
            ++total_connections_;
        }

        void
        track_destroyed()
        {
            if (total_connections_ > 0)
            {
                --total_connections_;
            }
        }

        void
        apply_client_settings(http_client& c) const
        {
            c.set_timeout_policy(cfg_.timeout_policy);
            c.set_timeout(cfg_.timeout);
            c.set_max_redirects(cfg_.max_redirects);
            c.set_verify_ssl(cfg_.verify_ssl);
            if (!cfg_.ca_cert.empty())
            {
                c.set_ca_cert(cfg_.ca_cert);
            }
            c.set_logger(logger());
        }

        std::deque<pooled_conn>::iterator
        close_idle_conn(std::deque<pooled_conn>& idle, std::deque<pooled_conn>::iterator it)
        {
            it->client->close();
            auto next = idle.erase(it);
            track_destroyed();
            return next;
        }

        void
        wake_one_waiter(std::string const& url)
        {
            auto it = waiters_.find(url);
            if (it == waiters_.end())
            {
                return;
            }
            auto& waiters = it->second;
            while (!waiters.empty())
            {
                auto w = std::move(waiters.front());
                waiters.pop_front();
                if (auto waiter = w.lock())
                {
                    waiter->timer.cancel();
                    break;
                }
            }
            if (waiters.empty())
            {
                waiters_.erase(it);
            }
        }

        client_handle
        acquire_or_create(std::string const& url)
        {
            auto it = pools_.find(url);
            if (it != pools_.end())
            {
                auto& st = it->second;
                while (!st.idle.empty())
                {
                    auto conn = std::move(st.idle.front().client);
                    st.idle.pop_front();

                    if (cfg_.validate_on_borrow && !conn->is_alive())
                    {
                        track_destroyed();
                        logger()->warn("client pool: discarding dead idle connection for {}", url);
                        continue;
                    }

                    st.active_count++;
                    ++total_active_;
                    return client_handle(shared_from_this(), std::move(conn));
                }
            }

            // Only touch pools_ when actually creating a connection, so a failed
            // acquire does not leave an empty pool_state entry behind.
            size_t active = 0;
            size_t idle = 0;
            if (it != pools_.end())
            {
                active = static_cast<size_t>(it->second.active_count);
                idle = it->second.idle.size();
            }
            if (active + idle < cfg_.max_size && (cfg_.max_total == 0 || total_connections_ < cfg_.max_total))
            {
                auto& st = pools_[url];
                st.active_count++;
                track_created();
                ++total_active_;
                auto client = std::make_unique<http_client>(ex_, url);
                apply_client_settings(*client);
                logger()->debug("client pool: created connection for {} (total={})", url, total_connections_);
                return client_handle(shared_from_this(), std::move(client));
            }

            return {};
        }

        net::awaitable<void>
        co_maintain(std::shared_ptr<net::steady_timer> timer)
        {
            boost::system::error_code ec;
            while (!stopped_)
            {
                auto timeout = cfg_.idle_timeout;
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
                        if (now - it2->idle_since > cfg_.idle_timeout)
                        {
                            logger()->trace("client pool: evicting idle connection for {}", it->first);
                            it2 = close_idle_conn(st.idle, it2);
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
        size_t total_connections_ = 0;
        size_t total_active_ = 0;
        pool_params cfg_;

        std::shared_ptr<spdlog::logger> default_logger_;
        std::shared_ptr<spdlog::logger> custom_logger_;

        std::unordered_map<std::string, waiters_list> waiters_;

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

    http_client_pool::http_client_pool(net::any_io_executor const& ex, pool_params params)
        : impl_(std::make_shared<impl>(ex, params))
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

    std::shared_ptr<spdlog::logger>
    http_client_pool::logger() const
    {
        return impl_->logger();
    }

    void
    http_client_pool::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        impl_->set_logger(std::move(logger));
    }

    size_t
    http_client_pool::active_count() const
    {
        return impl_->active_count();
    }

    size_t
    http_client_pool::idle_count() const
    {
        return impl_->idle_count();
    }

    size_t
    http_client_pool::total_count() const
    {
        return impl_->total_count();
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
