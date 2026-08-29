#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <memory>
#include <string_view>

namespace httplib::client
{

    class HTTPLIB_API proxy_client
    {
      public:
        explicit proxy_client(net::io_context& ex, std::string_view host, uint16_t port, bool ssl = false);
        explicit proxy_client(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl = false);
        ~proxy_client();

        net::awaitable<boost::system::error_code> async_connect(std::string_view target,
                                                                http::fields const& headers = {});

        net::awaitable<boost::system::result<std::size_t>> async_read_some(net::mutable_buffer const& buffer);
        net::awaitable<boost::system::error_code> async_write(net::const_buffer const& buffer);

        void close();
        void abort();
        bool is_open() const noexcept;

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        void set_verify_ssl(bool verify);
        void set_ca_cert(std::string_view cert);

      private:
        proxy_client(proxy_client const&) = delete;
        proxy_client& operator=(proxy_client const&) = delete;

        class impl;
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::client
