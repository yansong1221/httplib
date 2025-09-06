#pragma once
#include "httplib/client.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace httplib {

class multi_client_pool : public std::enable_shared_from_this<multi_client_pool>
{
private:
    struct ConnectionInfo
    {
        std::string host;
        uint16_t port;

        bool operator==(const ConnectionInfo& other) const
        {
            return host == other.host && port == other.port;
        }
        bool operator<(const ConnectionInfo& other) const
        {
            if (host == other.host)
                return port < other.port;
            else
                return host < other.host;
        }
    };

    std::map<ConnectionInfo, std::queue<std::unique_ptr<client>>> pools_;
    std::mutex mutex_;
    net::any_io_executor ex_;
    size_t max_size_;

public:
    class ClientHandle
    {
        std::weak_ptr<multi_client_pool> pool_;
        std::unique_ptr<client> conn_;

    public:
        ClientHandle(std::weak_ptr<multi_client_pool> pool, std::unique_ptr<client> conn)
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

        client* operator->() { return conn_.get(); }
        const client* operator->() const { return conn_.get(); }

        client& operator*() { return *conn_; }
        const client& operator*() const { return *conn_; }
    };

public:
    multi_client_pool(const net::any_io_executor& ex, size_t max_size = 10)
        : ex_(ex)
        , max_size_(max_size)
    {
    }

    ClientHandle acquire(std::string_view host, uint16_t port)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ConnectionInfo info {std::string(host), port};
        std::unique_ptr<client> conn;

        if (pools_.count(info) && !pools_[info].empty()) {
            conn = std::move(pools_[info].front());
            pools_[info].pop();
        }
        else {
            conn = std::make_unique<client>(ex_, host, port);
        }
        return ClientHandle(weak_from_this(), std::move(conn));
    }

    void release(std::unique_ptr<client> conn)
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

} // namespace httplib