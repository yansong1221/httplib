#pragma once
#include "httplib/client/proxy_client.hpp"
#include "stream/http_stream.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <spdlog/spdlog.h>

namespace httplib::client
{

    class proxy_client::impl : public std::enable_shared_from_this<impl>
    {
      public:
        impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl);

      public:
        net::awaitable<boost::system::error_code> async_connect(std::string_view target,
                                                                http::fields const& headers = {});

        net::awaitable<boost::system::result<std::size_t>> async_read_some(net::mutable_buffer const& buffer);
        net::awaitable<boost::system::error_code> async_write(net::const_buffer const& buffer);

        void close();
        void abort();
        bool is_open() const noexcept;

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        void
        set_verify_ssl(bool verify)
        {
            verify_ssl_ = verify;
        }
        void
        set_ca_cert(std::string_view cert)
        {
            ca_cert_ = cert;
        }

      private:
        net::any_io_executor executor_;
        tcp::resolver resolver_;
        std::string host_;
        uint16_t port_ = 0;
        bool use_ssl_ = false;
        bool verify_ssl_ = true;
        std::string ca_cert_;

        std::unique_ptr<http_stream> stream_;
        beast::flat_buffer buffer_;

        std::shared_ptr<spdlog::logger> default_logger_;
        std::shared_ptr<spdlog::logger> custom_logger_;
    };

} // namespace httplib::client
