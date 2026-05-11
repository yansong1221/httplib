#include "httplib/client/client_pool.hpp"
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>

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
        std::size_t seed = std::hash<std::string> {}(info.host);
        seed ^= std::hash<uint16_t> {}(info.port) + 0x9e3779b97f4a7c15ULL + (seed << 6) +
                (seed >> 2);
        seed ^= std::hash<bool> {}(info.ssl) + 0x9e3779b97f4a7c15ULL + (seed << 6) +
                (seed >> 2);
        return seed;
    }
};

using ClientQueue = std::queue<std::unique_ptr<http_client>>;

} // namespace

class http_client_pool::impl
{
public:
    impl(const net::any_io_executor& ex, size_t max_size)
        : ex_(ex)
        , max_size_(max_size)
    {
    }

    ~impl() { close_all(); }

    std::unique_ptr<http_client> acquire(std::string_view host, uint16_t port, bool ssl)
    {
        ConnectionInfo info {std::string(host), port, ssl};

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto& pool = pools_[info];
            if (!pool.empty()) {
                auto conn = std::move(pool.front());
                pool.pop();
                return conn;
            }
        }

        return std::make_unique<http_client>(ex_, info.host, info.port, info.ssl);
    }

    void release(std::unique_ptr<http_client> conn)
    {
        if (!conn)
            return;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            ConnectionInfo info {std::string(conn->host()), conn->port(), conn->is_use_ssl()};
            auto& pool = pools_[info];
            if (pool.size() < max_size_) {
                pool.push(std::move(conn));
                return;
            }
        }

        conn->close();
    }

    void close_all()
    {
        decltype(pools_) pending_close;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_close.swap(pools_);
        }

        for (auto& [info, pool] : pending_close) {
            while (!pool.empty()) {
                pool.front()->close();
                pool.pop();
            }
        }
    }

    std::unordered_map<ConnectionInfo, ClientQueue, ConnectionInfoHash> pools_;
    std::mutex mutex_;
    net::any_io_executor ex_;
    size_t max_size_;
};

http_client_pool::ClientHandle::ClientHandle(std::weak_ptr<impl> pool,
                                             std::unique_ptr<http_client> conn)
    : pool_(std::move(pool))
    , conn_(std::move(conn))
{
}

http_client_pool::ClientHandle::ClientHandle(ClientHandle&& other) noexcept
    : pool_(std::move(other.pool_))
    , conn_(std::move(other.conn_))
{
}

http_client_pool::ClientHandle&
http_client_pool::ClientHandle::operator=(ClientHandle&& other) noexcept
{
    if (this != &other) {
        release();
        pool_ = std::move(other.pool_);
        conn_ = std::move(other.conn_);
    }
    return *this;
}

http_client_pool::ClientHandle::~ClientHandle()
{
    release();
}

http_client* http_client_pool::ClientHandle::get() noexcept
{
    return conn_.get();
}

const http_client* http_client_pool::ClientHandle::get() const noexcept
{
    return conn_.get();
}

http_client_pool::ClientHandle::operator bool() const noexcept
{
    return static_cast<bool>(conn_);
}

http_client* http_client_pool::ClientHandle::operator->() noexcept
{
    return get();
}

const http_client* http_client_pool::ClientHandle::operator->() const noexcept
{
    return get();
}

http_client& http_client_pool::ClientHandle::operator*() noexcept
{
    return *conn_;
}

const http_client& http_client_pool::ClientHandle::operator*() const noexcept
{
    return *conn_;
}

void http_client_pool::ClientHandle::release()
{
    auto pool = pool_.lock();
    if (pool && conn_)
        pool->release(std::move(conn_));
}

http_client_pool::http_client_pool(const net::any_io_executor& ex, size_t max_size /*= 10*/)
    : impl_(std::make_shared<impl>(ex, max_size))
{
}

http_client_pool::~http_client_pool()
{
}

http_client_pool::ClientHandle http_client_pool::acquire(std::string_view host,
                                                         uint16_t port,
                                                         bool ssl /*= false*/)
{
    auto conn = impl_->acquire(host, port, ssl);
    return ClientHandle(impl_, std::move(conn));
}

net::any_io_executor http_client_pool::get_executor() noexcept
{
    return impl_->ex_;
}

} // namespace httplib::client
