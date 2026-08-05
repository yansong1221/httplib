#include "httplib/client/client_pool.hpp"
#include "httplib/client/client.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url.hpp>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace httplib::client
{

    namespace
    {

        struct connection_info
        {
            std::string host;
            uint16_t port;
            bool ssl;

            bool
            operator==(connection_info const& other) const
            {
                return host == other.host && port == other.port && ssl == other.ssl;
            }
        };

        struct connection_info_hash
        {
            size_t
            operator()(connection_info const& info) const noexcept
            {
                std::size_t seed = std::hash<std::string> {}(info.host);
                seed ^= std::hash<uint16_t> {}(info.port) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                seed ^= std::hash<bool> {}(info.ssl) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                return seed;
            }
        };

        struct pooled_conn
        {
            std::unique_ptr<http_client> client;
            std::chrono::steady_clock::time_point idle_since;
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
        impl(net::any_io_executor const& ex, size_t max_size, std::chrono::seconds idle_timeout)
            : ex_(ex)
            , max_size_(max_size)
            , idle_timeout_(idle_timeout)
            , cleanup_timer_(ex)
        {
        }

        ~impl() { stop(); }

        void
        start_cleanup()
        {
            net::co_spawn(
                ex_,
                [this, self = shared_from_this()]() -> net::awaitable<void> { co_await co_cleanup_expired(); },
                net::detached);
        }

        net::awaitable<client_handle>
        async_acquire(std::string_view host,
                      uint16_t port,
                      bool ssl,
                      std::chrono::milliseconds wait_timeout = std::chrono::milliseconds(0))
        {
            auto self = shared_from_this();
            connection_info info { std::string(host), port, ssl };
            auto deadline = std::chrono::steady_clock::now() + wait_timeout;

            do
            {
                std::unique_lock<std::mutex> lock(self->mutex_);
                if (self->stopped_)
                {
                    co_return client_handle(
                        boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
                }

                if (auto handle = self->try_pop(info); handle)
                {
                    co_return std::move(handle);
                }

                if (self->active_count_ < self->max_size_)
                {
                    self->active_count_++;
                    co_return client_handle(self,
                                            std::make_unique<http_client>(self->ex_, info.host, info.port, info.ssl));
                }
                if (wait_timeout <= std::chrono::milliseconds(0))
                {
                    co_return client_handle(boost::system::errc::make_error_code(boost::system::errc::timed_out));
                }

                auto node = std::make_shared<waiter_node>(self->ex_);

                node->timer.expires_at(deadline);
                self->waiters_.push_back(node);
                lock.unlock();

                auto wait_start = std::chrono::steady_clock::now();
                boost::system::error_code ec;
                co_await node->timer.async_wait(util::net_awaitable[ec]);

                lock.lock();

                if (auto handle = self->try_pop(info); handle)
                {
                    co_return std::move(handle);
                }
            } while (deadline > std::chrono::steady_clock::now());

            co_return client_handle(boost::system::errc::make_error_code(boost::system::errc::timed_out));
        }

        void
        release(std::unique_ptr<http_client> conn)
        {
            if (!conn || conn->has_active_session())
            {
                return;
            }

            connection_info info { std::string(conn->host()), conn->port(), conn->is_use_ssl() };

            std::lock_guard<std::mutex> lock(mutex_);

            if (stopped_)
            {
                return;
            }

            if (active_count_ > 0)
            {
                active_count_--;
            }

            auto& pool = pools_[info];
            if (pool.size() < max_size_)
            {
                pool.push_back({ std::move(conn), std::chrono::steady_clock::now() });
            }

            while (!waiters_.empty())
            {
                auto w = waiters_.front();
                waiters_.pop_front();
                if (w.expired())
                {
                    continue;
                }
                if (auto waiter = w.lock(); waiter)
                {
                    waiter->timer.cancel();
                    break;
                }
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
            cleanup_timer_.cancel();

            waiters_list waiters;
            waiters.swap(waiters_);

            for (auto& [info, pool] : pools_)
            {
                for (auto& pc : pool)
                {
                    pc.client->close();
                }
                pool.clear();
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
                    waiter->timer.expires_after(std::chrono::seconds(0));
                }
            }
        }

        pool_stats
        stats(connection_info const& info) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pool_stats s;
            auto it = pools_.find(info);
            if (it != pools_.end())
            {
                s.idle = it->second.size();
            }
            s.active = active_count_;
            return s;
        }

        void
        set_max_size(size_t n)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            max_size_ = n;
            // Trim excess connections
            for (auto& [info, pool] : pools_)
            {
                while (pool.size() > max_size_)
                {
                    pool.front().client->close();
                    pool.erase(pool.begin());
                }
            }
        }

        void
        set_idle_timeout(std::chrono::seconds timeout)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            idle_timeout_ = timeout;
        }

      private:
        client_handle
        try_pop(connection_info const& info)
        {
            auto it = pools_.find(info);
            if (it != pools_.end() && !it->second.empty())
            {
                auto& pool = it->second;
                auto conn = std::move(pool.front().client);
                pool.erase(pool.begin());
                if (pool.empty())
                {
                    pools_.erase(it);
                }
                active_count_++;
                return client_handle(shared_from_this(), std::move(conn));
            }
            return {};
        }

        net::awaitable<void>
        co_cleanup_expired()
        {
            boost::system::error_code ec;
            while (!stopped_)
            {
                cleanup_timer_.expires_after(idle_timeout_);
                co_await cleanup_timer_.async_wait(util::net_awaitable[ec]);
                if (ec || stopped_)
                {
                    co_return;
                }

                auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto it = pools_.begin(); it != pools_.end();)
                {
                    auto& pool = it->second;
                    while (!pool.empty())
                    {
                        auto& front = pool.front();
                        if (now - front.idle_since > idle_timeout_)
                        {
                            front.client->close();
                            pool.erase(pool.begin());
                            continue;
                        }
                        break;
                    }
                    if (pool.empty())
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
        using pool_list = std::vector<pooled_conn>;
        using waiters_list = std::deque<std::weak_ptr<waiter_node>>;

        mutable std::mutex mutex_;
        std::unordered_map<connection_info, pool_list, connection_info_hash> pools_;
        size_t max_size_;
        std::chrono::seconds idle_timeout_;
        size_t active_count_ = 0;

        waiters_list waiters_;

      public:
        net::any_io_executor ex_; // for get_executor()

      private:
        std::atomic<bool> stopped_ { false };
        net::steady_timer cleanup_timer_;
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

    void
    http_client_pool::client_handle::release()
    {
        auto pool = pool_.lock();
        if (pool && conn_)
        {
            pool->release(std::move(conn_));
        }
    }

    // ---- pool_list ----

    http_client_pool::http_client_pool(net::any_io_executor const& ex,
                                       size_t max_size /*= 10*/,
                                       std::chrono::seconds idle_timeout /*= 60s*/)
        : impl_(std::make_shared<impl>(ex, max_size, idle_timeout))
    {
        impl_->start_cleanup();
    }

    http_client_pool::~http_client_pool() { impl_->stop(); }

    std::future<http_client_pool::client_handle>
    http_client_pool::acquire(std::string_view host,
                              uint16_t port,
                              bool ssl /*= false*/,
                              std::chrono::milliseconds wait_timeout /*= std::chrono::milliseconds(0)*/)
    {
        return net::co_spawn(
            get_executor(),
            [this, h = std::string(host), port, ssl, wait_timeout]() -> net::awaitable<client_handle>
            { co_return co_await async_acquire(h, port, ssl, wait_timeout); },
            net::use_future);
    }

    net::awaitable<http_client_pool::client_handle>
    http_client_pool::async_acquire(std::string_view host,
                                    uint16_t port,
                                    bool ssl /*= false*/,
                                    std::chrono::milliseconds wait_timeout /*= std::chrono::milliseconds(0)*/)
    {
        co_return co_await impl_->async_acquire(host, port, ssl, wait_timeout);
    }

    std::future<http_client_pool::client_handle>
    http_client_pool::acquire(std::string_view url,
                              std::chrono::milliseconds wait_timeout /*= std::chrono::milliseconds(0)*/)
    {
        return net::co_spawn(
            get_executor(),
            [this, u = std::string(url), wait_timeout]() -> net::awaitable<client_handle>
            { co_return co_await async_acquire(u, wait_timeout); },
            net::use_future);
    }

    net::awaitable<http_client_pool::client_handle>
    http_client_pool::async_acquire(std::string_view url,
                                    std::chrono::milliseconds wait_timeout /*= std::chrono::milliseconds(0)*/)
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
    http_client_pool::set_idle_timeout(std::chrono::seconds timeout)
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
        connection_info info { std::string(host), port, ssl };
        return impl_->stats(info);
    }

} // namespace httplib::client
