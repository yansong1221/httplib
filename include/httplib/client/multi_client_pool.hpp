#pragma once
#include "httplib/client/client.hpp"
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

namespace httplib::client {

class multi_http_client_pool : public std::enable_shared_from_this<multi_http_client_pool>
{
private:
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
            std::size_t h1 = std::hash<std::string> {}(info.host);
            std::size_t h2 = std::hash<uint16_t> {}(info.port);
            std::size_t h3 = std::hash<bool> {}(info.ssl);

            // 64-bit hash combine（比直接 XOR 更好）
            std::size_t seed = h1;
            seed ^= h2 + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            seed ^= h3 + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            return seed;
        }
    };


    std::unordered_map<ConnectionInfo, std::queue<std::unique_ptr<http_client>>, ConnectionInfoHash>
        pools_;
    std::mutex mutex_;
    net::any_io_executor ex_;
    size_t max_size_;

public:
    class ClientHandle
    {
        std::weak_ptr<multi_http_client_pool> pool_;
        std::unique_ptr<http_client> conn_;

    public:
        ClientHandle(std::weak_ptr<multi_http_client_pool> pool, std::unique_ptr<http_client> conn)
            : pool_(pool)
            , conn_(std::move(conn))
        {
        }

        ClientHandle(const ClientHandle&)            = delete;
        ClientHandle& operator=(const ClientHandle&) = delete;

        ClientHandle(ClientHandle&& other) noexcept = default;
        ClientHandle& operator=(ClientHandle&&)     = default;

        ~ClientHandle()
        {
            auto ptr = pool_.lock();
            if (conn_ && ptr) {
                ptr->release(std::move(conn_));
            }
        }

        http_client* operator->() { return conn_.get(); }
        const http_client* operator->() const { return conn_.get(); }

        http_client& operator*() { return *conn_; }
        const http_client& operator*() const { return *conn_; }
    };

public:
    multi_http_client_pool(const net::any_io_executor& ex, size_t max_size = 10)
        : ex_(ex)
        , max_size_(max_size)
    {
    }

    ClientHandle acquire(std::string_view host, uint16_t port, bool ssl = false)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ConnectionInfo info {std::string(host), port, ssl};
        std::unique_ptr<http_client> conn;

        if (pools_.count(info) && !pools_[info].empty()) {
            conn = std::move(pools_[info].front());
            pools_[info].pop();
        }
        else {
            conn = std::make_unique<http_client>(ex_, host, port, ssl);
        }
        return ClientHandle(weak_from_this(), std::move(conn));
    }

    void release(std::unique_ptr<http_client> conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ConnectionInfo info {std::string(conn->host()),
                             conn->port()}; // Assuming client has host() and port() methods
        if (pools_[info].size() < max_size_) {
            pools_[info].push(std::move(conn));
        }
    }

    net::any_io_executor get_executor() noexcept { return ex_; }
};

} // namespace httplib::client
