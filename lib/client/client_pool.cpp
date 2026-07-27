#include "httplib/client/client_pool.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <algorithm>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace httplib::client {

namespace {

struct connection_info
{
    std::string host;
    uint16_t port;
    bool ssl;

    bool operator==(const connection_info& other) const
    {
        return host == other.host && port == other.port && ssl == other.ssl;
    }
};

struct connection_info_hash
{
    size_t operator()(const connection_info& info) const noexcept
    {
        std::size_t seed = std::hash<std::string> {}(info.host);
        seed ^=
            std::hash<uint16_t> {}(info.port) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
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

    explicit waiter_node(const net::any_io_executor& ex)
        : timer(ex)
    {
    }
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
        net::co_spawn(
            ex_,
            [this, self = shared_from_this()]() -> net::awaitable<void> {
                co_await co_cleanup_expired();
            },
            net::detached);
    }

    net::awaitable<client_handle>
    async_acquire(std::string_view host, uint16_t port, bool ssl, bool wait = true)
    {
        connection_info info {std::string(host), port, ssl};
        auto node = std::make_shared<waiter_node>(ex_);

        for (;;) {
            bool should_create = false;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (stopped_)
                    co_return client_handle(boost::system::errc::make_error_code(
                        boost::system::errc::operation_canceled));

                auto it = pools_.find(info);
                if (it != pools_.end() && !it->second.empty()) {
                    auto& pool = it->second;
                    auto conn  = std::move(pool.front().client);
                    pool.erase(pool.begin());
                    if (pool.empty())
                        pools_.erase(it);
                    active_count_++;
                    co_return client_handle(shared_from_this(), std::move(conn));
                }

                if (!wait || active_count_ < max_size_) {
                    active_count_++;
                    should_create = true;
                }
                else {
                    node->timer.expires_at(std::chrono::steady_clock::time_point::max());
                    waiters_.push_back(node);
                }
            }

            if (should_create)
                co_return client_handle(
                    shared_from_this(),
                    std::make_unique<http_client>(ex_, info.host, info.port, info.ssl));

            boost::system::error_code ec;
            co_await node->timer.async_wait(util::net_awaitable[ec]);
        }
    }

    void release(std::unique_ptr<http_client> conn)
    {
        if (!conn)
            return;

        connection_info info {std::string(conn->host()), conn->port(), conn->is_use_ssl()};

        std::shared_ptr<waiter_node> w;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (stopped_) {
                conn->close();
                return;
            }

            if (active_count_ > 0)
                active_count_--;

            auto& pool = pools_[info];
            if (pool.size() < max_size_) {
                pool.push_back({std::move(conn), std::chrono::steady_clock::now()});
            }
            else {
                conn->close();
            }

            while (!waiters_.empty()) {
                w = waiters_.front();
                waiters_.pop_front();
                break;
            }
        }

        if (w)
            w->timer.cancel();
    }

    void close_all()
    {
        if (stopped_.exchange(true))
            return;

        cleanup_timer_.cancel();

        std::vector<std::shared_ptr<waiter_node>> waiters_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waiters_copy.insert(waiters_copy.end(), waiters_.begin(), waiters_.end());
            waiters_.clear();

            for (auto& [info, pool] : pools_) {
                for (auto& pc : pool)
                    pc.client->close();
                pool.clear();
            }
            pools_.clear();
        }

        for (auto& w : waiters_copy) {
            w->timer.cancel();
            w->timer.expires_after(std::chrono::seconds(0));
        }
    }

    pool_stats stats(const connection_info& info) const
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
    using pool_list    = std::vector<pooled_conn>;
    using waiters_list = std::deque<std::shared_ptr<waiter_node>>;

    mutable std::mutex mutex_;
    std::unordered_map<connection_info, pool_list, connection_info_hash> pools_;
    size_t max_size_;
    std::chrono::seconds idle_timeout_;
    size_t active_count_ = 0;

    waiters_list waiters_;

public:
    net::any_io_executor ex_; // for get_executor()

private:
    std::atomic<bool> stopped_ {false};
    net::steady_timer cleanup_timer_;
};

// ---- client_handle ----

http_client_pool::client_handle::client_handle(std::weak_ptr<impl> pool,
                                               std::unique_ptr<http_client> conn)
    : pool_(std::move(pool))
    , conn_(std::move(conn))
{
}

http_client_pool::client_handle::client_handle(boost::system::error_code ec)
    : error_(ec)
{
}

http_client_pool::client_handle::client_handle(client_handle&& other) noexcept
    : pool_(std::move(other.pool_))
    , conn_(std::move(other.conn_))
    , error_(other.error_)
{
}

http_client_pool::client_handle&
http_client_pool::client_handle::operator=(client_handle&& other) noexcept
{
    if (this != &other) {
        release();
        pool_  = std::move(other.pool_);
        conn_  = std::move(other.conn_);
        error_ = other.error_;
    }
    return *this;
}

http_client_pool::client_handle::~client_handle()
{
    release();
}

http_client* http_client_pool::client_handle::get()
{
    if (!conn_)
        throw boost::system::system_error(error_);
    return conn_.get();
}

const http_client* http_client_pool::client_handle::get() const
{
    if (!conn_)
        throw boost::system::system_error(error_);
    return conn_.get();
}

http_client_pool::client_handle::operator bool() const noexcept
{
    return !has_error();
}

bool http_client_pool::client_handle::has_error() const noexcept
{
    return conn_ == nullptr;
}

http_client* http_client_pool::client_handle::operator->()
{
    return get();
}

const http_client* http_client_pool::client_handle::operator->() const
{
    return get();
}

http_client& http_client_pool::client_handle::operator*()
{
    return *get();
}

const http_client& http_client_pool::client_handle::operator*() const
{
    return *get();
}

const boost::system::error_code& http_client_pool::client_handle::error() const noexcept
{
    return error_;
}

void http_client_pool::client_handle::release()
{
    auto pool = pool_.lock();
    if (pool && conn_)
        pool->release(std::move(conn_));
}

// ---- pool_list ----

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

std::future<http_client_pool::client_handle> http_client_pool::acquire(std::string_view host,
                                                                       uint16_t port,
                                                                       bool ssl /*= false*/)
{
    return net::co_spawn(
        get_executor(),
        [this, h = std::string(host), port, ssl]() -> net::awaitable<client_handle> {
            co_return co_await async_acquire(h, port, ssl);
        },
        net::use_future);
}

net::awaitable<http_client_pool::client_handle>
http_client_pool::async_acquire(std::string_view host, uint16_t port, bool ssl /*= false*/)
{
    co_return co_await impl_->async_acquire(host, port, ssl);
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

void http_client_pool::stop()
{
    impl_->close_all();
}

http_client_pool::pool_stats http_client_pool::stats(std::string_view host,
                                                     uint16_t port,
                                                     bool ssl /*= false*/) const
{
    connection_info info {std::string(host), port, ssl};
    return impl_->stats(info);
}

} // namespace httplib::client
