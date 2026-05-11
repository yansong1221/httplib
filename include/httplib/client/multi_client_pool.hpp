#pragma once
#include "httplib/client/client.hpp"
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

namespace httplib::client {

class HTTPLIB_API multi_http_client_pool
    : public std::enable_shared_from_this<multi_http_client_pool>
{
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
    multi_http_client_pool(const net::any_io_executor& ex, size_t max_size = 10);
    ~multi_http_client_pool();

    ClientHandle acquire(std::string_view host, uint16_t port, bool ssl = false);
    void release(std::unique_ptr<http_client> conn);

    net::any_io_executor get_executor() noexcept { return ex_; }

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
};

} // namespace httplib::client
