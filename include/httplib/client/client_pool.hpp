#pragma once
#include "httplib/client/client.hpp"
#include <queue>
namespace httplib::client {

class http_client_pool : public std::enable_shared_from_this<http_client_pool>
{
private:
    std::queue<std::unique_ptr<http_client>> pool_;
    std::mutex mutex_;
    net::any_io_executor ex_;
    std::string host_;
    uint16_t port_;
    size_t max_size_;
    bool ssl_;

public:
    class ClientHandle
    {
        std::weak_ptr<http_client_pool> pool_;
        std::unique_ptr<http_client> conn_;

    public:
        ClientHandle(std::weak_ptr<http_client_pool> pool, std::unique_ptr<http_client> conn)
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
    http_client_pool(const net::any_io_executor& ex,
                     std::string_view host,
                     uint16_t port,
                     bool ssl        = false,
                     size_t max_size = 10)
        : ex_(ex)
        , host_(host)
        , port_(port)
        , max_size_(max_size)
        , ssl_(ssl)
    {
    }
    ~http_client_pool()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pool_.empty()) {
            pool_.front()->close();
            pool_.pop();
        }
    }

public:
    std::string_view host() const { return host_; }
    uint16_t port() const { return port_; }

    ClientHandle acquire()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::unique_ptr<http_client> conn;
        if (!pool_.empty()) {
            conn = std::move(pool_.front());
            pool_.pop();
        }
        else {
            conn = std::make_unique<http_client>(ex_, host_, port_, ssl_);
        }
        return ClientHandle(weak_from_this(), std::move(conn));
    }

    void release(std::unique_ptr<http_client> conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pool_.size() < max_size_) {
            pool_.push(std::move(conn));
        }
    }
    net::any_io_executor get_executor() noexcept { return ex_; }
};
} // namespace httplib::client