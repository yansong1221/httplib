#include "httplib/client/client_pool.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <algorithm>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace httplib::client {

namespace {

struct ConnectionInfo
{
    std::string host;
    uint16_t port;
    bool ssl;

    bool operator==(const ConnectionInfo& other) const
    {
        return host == other.host && port == other.port && ssl == other.ssl;
    }
};

struct ConnectionInfoHash
{
    size_t operator()(const ConnectionInfo& info) const noexcept
    {
        std::size_t seed = std::hash<std::string>{}(info.host);
        seed ^= std::hash<uint16_t>{}(info.port) + 0x9e3779b97f4a7c15ULL + (seed << 6) +
                (seed >> 2);
        seed ^= std::hash<bool>{}(info.ssl) + 0x9e3779b97f4a7c15ULL + (seed << 6) +
                (seed >> 2);
        return seed;
    }
};

struct PooledConn
{
    std::unique_ptr<http_client> client;
    std::chrono::steady_clock::time_point idle_since;
};

} // namespace

class http_client_pool::impl : public std::enable_shared_from_this<impl>
{
public:
    impl(const net::any_io_executor& ex, size_t max_size, std::chrono::seconds idle_timeout)
        : ex_(ex)
        , max_size_(max_size)
        , idle_timeout_(idle_timeout)
        , cleanup_timer_(ex)
    {
    }

    ~impl() { close_all(); }

    void start_cleanup()
    {
        net::co_spawn(ex_,
            [this, self = shared_from_this()]() -> net::awaitable<void> {
                co_await co_cleanup_expired();
            },
            net::detached);
    }

    std::unique_ptr<http_client> acquire(std::string_view host, uint16_t port, bool ssl)
    {
        ConnectionInfo info{std::string(host), port, ssl};

        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pools_.find(info);
            if (it != pools_.end()) {
                auto& pool = it->second;
                // Evict stale connections from the front
                while (!pool.empty()) {
                    auto& front = pool.front();
                    if (now - front.idle_since > idle_timeout_) {
                        front.client->close();
                        pool.erase(pool.begin());
                        continue;
                    }
                    auto conn = std::move(front.client);
                    pool.erase(pool.begin());
                    return conn;
                }
                if (pool.empty())
                    pools_.erase(it);
            }
        }

        // No valid idle connection — create new
        return std::make_unique<http_client>(ex_, info.host, info.port, info.ssl);
    }

    void release(std::unique_ptr<http_client> conn)
    {
        if (!conn)
            return;

        ConnectionInfo info{std::string(conn->host()), conn->port(), conn->is_use_ssl()};

        std::lock_guard<std::mutex> lock(mutex_);

        if (active_count_ > 0)
            active_count_--;

        auto& pool = pools_[info];
        if (pool.size() < max_size_) {
            pool.push_back({std::move(conn), std::chrono::steady_clock::now()});
            return;
        }

        conn->close();
    }

    void close_all()
    {
        stopped_ = true;
        cleanup_timer_.cancel();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [info, pool] : pools_) {
            for (auto& pc : pool)
                pc.client->close();
            pool.clear();
        }
        pools_.clear();
    }

    pool_stats stats(const ConnectionInfo& info) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_stats s;
        auto it = pools_.find(info);
        if (it != pools_.end())
            s.idle = it->second.size();
        s.active = active_count_;
        return s;
    }

    void set_max_size(size_t n)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        max_size_ = n;
        // Trim excess connections
        for (auto& [info, pool] : pools_) {
            while (pool.size() > max_size_) {
                pool.front().client->close();
                pool.erase(pool.begin());
            }
        }
    }

    void set_idle_timeout(std::chrono::seconds timeout)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        idle_timeout_ = timeout;
    }

private:
    net::awaitable<void> co_cleanup_expired()
    {
        boost::system::error_code ec;
        while (!stopped_) {
            cleanup_timer_.expires_after(idle_timeout_);
            co_await cleanup_timer_.async_wait(util::net_awaitable[ec]);
            if (ec || stopped_)
                co_return;

            auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = pools_.begin(); it != pools_.end();) {
                auto& pool = it->second;
                while (!pool.empty()) {
                    auto& front = pool.front();
                    if (now - front.idle_since > idle_timeout_) {
                        front.client->close();
                        pool.erase(pool.begin());
                        continue;
                    }
                    break;
                }
                if (pool.empty())
                    it = pools_.erase(it);
                else
                    ++it;
            }
        }
    }

private:
    using Pool = std::vector<PooledConn>;

    mutable std::mutex mutex_;
    std::unordered_map<ConnectionInfo, Pool, ConnectionInfoHash> pools_;
    size_t max_size_;
    std::chrono::seconds idle_timeout_;
    size_t active_count_ = 0;

public:
    net::any_io_executor ex_;  // for get_executor()

private:
    std::atomic<bool> stopped_{false};
    net::steady_timer cleanup_timer_;
};

// ---- client_handle ----

http_client_pool::client_handle::client_handle(std::weak_ptr<impl> pool,
                                              std::unique_ptr<http_client> conn)
    : pool_(std::move(pool))
    , conn_(std::move(conn))
{
}

http_client_pool::client_handle::client_handle(client_handle&& other) noexcept
    : pool_(std::move(other.pool_))
    , conn_(std::move(other.conn_))
{
}

http_client_pool::client_handle&
http_client_pool::client_handle::operator=(client_handle&& other) noexcept
{
    if (this != &other) {
        release();
        pool_ = std::move(other.pool_);
        conn_ = std::move(other.conn_);
    }
    return *this;
}

http_client_pool::client_handle::~client_handle()
{
    release();
}

http_client* http_client_pool::client_handle::get() noexcept
{
    return conn_.get();
}

const http_client* http_client_pool::client_handle::get() const noexcept
{
    return conn_.get();
}

http_client_pool::client_handle::operator bool() const noexcept
{
    return static_cast<bool>(conn_);
}

http_client* http_client_pool::client_handle::operator->() noexcept
{
    return get();
}

const http_client* http_client_pool::client_handle::operator->() const noexcept
{
    return get();
}

http_client& http_client_pool::client_handle::operator*() noexcept
{
    return *conn_;
}

const http_client& http_client_pool::client_handle::operator*() const noexcept
{
    return *conn_;
}

void http_client_pool::client_handle::release()
{
    auto pool = pool_.lock();
    if (pool && conn_)
        pool->release(std::move(conn_));
}

// ---- Pool ----

http_client_pool::http_client_pool(const net::any_io_executor& ex,
                                    size_t max_size /*= 10*/,
                                    std::chrono::seconds idle_timeout /*= 60s*/)
    : impl_(std::make_shared<impl>(ex, max_size, idle_timeout))
{
    impl_->start_cleanup();
}

http_client_pool::~http_client_pool()
{
    impl_->close_all();
}

http_client_pool::client_handle http_client_pool::acquire(std::string_view host,
                                                          uint16_t port,
                                                          bool ssl /*= false*/)
{
    auto conn = impl_->acquire(host, port, ssl);
    return client_handle(impl_, std::move(conn));
}

net::any_io_executor http_client_pool::get_executor() noexcept
{
    return impl_->ex_;
}

void http_client_pool::set_max_size(size_t n)
{
    impl_->set_max_size(n);
}

void http_client_pool::set_idle_timeout(std::chrono::seconds timeout)
{
    impl_->set_idle_timeout(timeout);
}

http_client_pool::pool_stats http_client_pool::stats(std::string_view host,
                                                  uint16_t port,
                                                  bool ssl /*= false*/) const
{
    ConnectionInfo info{std::string(host), port, ssl};
    return impl_->stats(info);
}

} // namespace httplib::client
