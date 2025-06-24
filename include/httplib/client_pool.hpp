#pragma once
#include "httplib/client.hpp"
#include <queue>
namespace httplib {

class client_pool : public std::enable_shared_from_this<client_pool>
{
private:
    std::queue<std::unique_ptr<client>> pool_;
    std::mutex mutex_;
    net::any_io_executor ex_;
    std::string host_;
    uint16_t port_;
    size_t max_size_;

public:
    class ClientHandle
    {
        std::weak_ptr<client_pool> pool_;
        std::unique_ptr<client> conn_;

    public:
        ClientHandle(std::weak_ptr<client_pool> pool, std::unique_ptr<client> conn)
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
    client_pool(const net::any_io_executor& ex,
                std::string_view host,
                uint16_t port,
                size_t max_size)
        : ex_(ex)
        , host_(host)
        , port_(port)
        , max_size_(max_size)
    {
    }
    ~client_pool()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pool_.empty()) {
            pool_.front()->close();
            pool_.pop();
        }
    }

public:
    ClientHandle acquire()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::unique_ptr<client> conn;
        if (!pool_.empty()) {
            conn = std::move(pool_.front());
            pool_.pop();
        }
        else {
            conn = std::make_unique<client>(ex_, host_, port_);
        }
        return ClientHandle(weak_from_this(), std::move(conn));
    }

    void release(std::unique_ptr<client> conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pool_.size() < max_size_) {
            pool_.push(std::move(conn));
        }
    }
    net::any_io_executor get_executor() noexcept { return ex_; }
};
} // namespace httplib