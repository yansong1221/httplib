#pragma once
#include "httplib/client/client.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <future>
#include <memory>

namespace httplib::client {

class HTTPLIB_API http_client_pool
{
private:
    class impl;

public:
    class HTTPLIB_API client_handle
    {
        friend class http_client_pool;

        std::weak_ptr<impl> pool_;
        std::unique_ptr<http_client> conn_;

        client_handle(std::weak_ptr<impl> pool, std::unique_ptr<http_client> conn);
        void release();

    public:
        client_handle()                                = default;
        client_handle(const client_handle&)            = delete;
        client_handle& operator=(const client_handle&) = delete;

        client_handle(client_handle&& other) noexcept;
        client_handle& operator=(client_handle&& other) noexcept;

        ~client_handle();

        http_client* get() noexcept;
        const http_client* get() const noexcept;

        explicit operator bool() const noexcept;
        http_client* operator->() noexcept;
        const http_client* operator->() const noexcept;

        http_client& operator*() noexcept;
        const http_client& operator*() const noexcept;
    };

    struct pool_stats
    {
        size_t idle   = 0;
        size_t active = 0;
    };

    using handle_result = boost::system::result<client_handle>;

public:
    explicit http_client_pool(const net::any_io_executor& ex,
                              size_t max_size                   = 10,
                              std::chrono::seconds idle_timeout = std::chrono::seconds(60));
    ~http_client_pool();

    http_client_pool(const http_client_pool&)            = delete;
    http_client_pool& operator=(const http_client_pool&) = delete;

    std::future<handle_result> acquire(std::string_view host, uint16_t port, bool ssl = false);

    net::awaitable<handle_result> async_acquire(std::string_view host,
                                                                        uint16_t port,
                                                                        bool ssl = false);

    net::any_io_executor get_executor() noexcept;

    void set_max_size(size_t n);
    void set_idle_timeout(std::chrono::seconds timeout);

    pool_stats stats(std::string_view host, uint16_t port, bool ssl = false) const;

private:
    std::shared_ptr<impl> impl_;
};

} // namespace httplib::client
