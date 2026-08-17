#pragma once
#include "httplib/client/client_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <future>
#include <memory>

namespace httplib::client
{

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
        explicit http_client_pool(net::any_io_executor const& ex,
                                  size_t max_size = 100,
                                  std::chrono::steady_clock::duration idle_timeout = std::chrono::seconds(60));
        ~http_client_pool();

        http_client_pool(http_client_pool const&) = delete;
        http_client_pool& operator=(http_client_pool const&) = delete;

        http_client_pool(http_client_pool&& other) noexcept;
        http_client_pool& operator=(http_client_pool&& other) noexcept;

        static constexpr auto default_timeout = std::chrono::seconds(3);

        std::future<client_handle> acquire(std::string_view host,
                                           uint16_t port,
                                           bool ssl,
                                           std::chrono::steady_clock::duration wait_timeout = default_timeout);

        std::future<client_handle> acquire(std::string_view url,
                                           std::chrono::steady_clock::duration wait_timeout = default_timeout);

        net::awaitable<client_handle> async_acquire(std::string_view host,
                                                    uint16_t port,
                                                    bool ssl,
                                                    std::chrono::steady_clock::duration wait_timeout = default_timeout);

        net::awaitable<client_handle> async_acquire(std::string_view url,
                                                    std::chrono::steady_clock::duration wait_timeout = default_timeout);

        net::any_io_executor get_executor() noexcept;

        void set_max_size(size_t n);
        void set_idle_timeout(std::chrono::steady_clock::duration timeout);

        void start();
        void stop();

        pool_stats stats(std::string_view host, uint16_t port, bool ssl) const;
        pool_stats stats(std::string_view url) const;

      private:
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::client
