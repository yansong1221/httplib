#pragma once
#include "httplib/client/client.hpp"
#include <memory>

namespace httplib::client {

class HTTPLIB_API http_client_pool
{
private:
    class impl;

public:
    class HTTPLIB_API ClientHandle
    {
        friend class http_client_pool;

        std::weak_ptr<impl> pool_;
        std::unique_ptr<http_client> conn_;

        ClientHandle(std::weak_ptr<impl> pool, std::unique_ptr<http_client> conn);
        void release();

    public:
        ClientHandle(const ClientHandle&)            = delete;
        ClientHandle& operator=(const ClientHandle&) = delete;

        ClientHandle(ClientHandle&& other) noexcept;
        ClientHandle& operator=(ClientHandle&& other) noexcept;

        ~ClientHandle();

        http_client* get() noexcept;
        const http_client* get() const noexcept;

        explicit operator bool() const noexcept;
        http_client* operator->() noexcept;
        const http_client* operator->() const noexcept;

        http_client& operator*() noexcept;
        const http_client& operator*() const noexcept;
    };

public:
    explicit http_client_pool(const net::any_io_executor& ex, size_t max_size = 10);
    ~http_client_pool();

    ClientHandle acquire(std::string_view host, uint16_t port, bool ssl = false);

    net::any_io_executor get_executor() noexcept;

private:
    std::shared_ptr<impl> impl_;
};

} // namespace httplib::client
