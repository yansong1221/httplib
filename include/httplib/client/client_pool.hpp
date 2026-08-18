#pragma once
#include "httplib/client/client.hpp"
#include "httplib/client/client_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace httplib::client
{

    /**
     * \brief HTTP 客户端连接池参数。
     * \details \c max_size 为每个 host（route）的连接数上限，\c max_total 为全局连接数上限（0 表示不限）。
     * 其余字段在新建连接时透传给 \c http_client。
     */
    struct pool_params
    {
        size_t max_size = 100;
        size_t max_total = 0;
        std::chrono::steady_clock::duration idle_timeout = std::chrono::seconds(60);
        bool validate_on_borrow = false;

        http_client::timeout_policy timeout_policy = http_client::timeout_policy::overall;
        std::chrono::steady_clock::duration timeout = std::chrono::seconds(30);
        int max_redirects = 0;
        bool verify_ssl = true;
        std::string ca_cert;
    };

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
            boost::system::error_code error_;

            client_handle(std::weak_ptr<impl> pool, std::unique_ptr<http_client> conn);

          public:
            client_handle();
            explicit client_handle(boost::system::error_code ec);

            client_handle(client_handle const&) = delete;
            client_handle& operator=(client_handle const&) = delete;

            client_handle(client_handle&& other) noexcept;
            client_handle& operator=(client_handle&& other) noexcept;

            ~client_handle();

            http_client* get();
            http_client const* get() const;

            explicit operator bool() const noexcept;
            bool has_error() const noexcept;

            http_client* operator->();
            http_client const* operator->() const;

            http_client& operator*();
            http_client const& operator*() const;

            boost::system::error_code const& error() const noexcept;

            void release();
        };

        struct pool_stats
        {
            size_t idle = 0;
            size_t active = 0;
        };

      public:
        explicit http_client_pool(net::any_io_executor const& ex, pool_params params = {});
        ~http_client_pool();

        http_client_pool(http_client_pool const&) = delete;
        http_client_pool& operator=(http_client_pool const&) = delete;

        http_client_pool(http_client_pool&& other) noexcept;
        http_client_pool& operator=(http_client_pool&& other) noexcept;

        // wait_timeout controls how long async_acquire() waits for a connection when
        // the pool is at capacity. A value of zero (or negative) makes the call fail
        // fast: it returns a handle carrying `timed_out` immediately if no
        // connection is available without waiting.
        static constexpr auto default_timeout = std::chrono::seconds(3);

        net::awaitable<client_handle> async_acquire(std::string_view host,
                                                    uint16_t port,
                                                    bool ssl,
                                                    std::chrono::steady_clock::duration wait_timeout = default_timeout);

        net::awaitable<client_handle> async_acquire(std::string_view url,
                                                    std::chrono::steady_clock::duration wait_timeout = default_timeout);

        net::any_io_executor get_executor() noexcept;

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        size_t active_count() const;
        size_t idle_count() const;
        size_t total_count() const;

        void start();
        void stop();

        pool_stats stats(std::string_view host, uint16_t port, bool ssl) const;
        pool_stats stats(std::string_view url) const;

      private:
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::client
