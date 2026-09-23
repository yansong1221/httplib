#pragma once
#include "httplib/client/proxy_client.hpp"
#include "stream/http_stream.hpp"
#include "util/logging.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <future>

namespace httplib::client
{

    class proxy_client::impl
        : public httplib::detail::logger
        , public std::enable_shared_from_this<impl>
    {
      public:
        impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, scheme s);

      public:
        net::awaitable<void> async_connect(std::string_view target,
                                           http::fields const& headers,
                                           boost::system::error_code& ec);

        net::awaitable<std::size_t> async_read_some(net::mutable_buffer const& buffer, boost::system::error_code& ec);

        net::awaitable<void> async_write(net::const_buffer const& buffer, boost::system::error_code& ec);

        std::future<void> close();
        net::awaitable<void> async_close();

        bool is_open() const noexcept;

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
        std::shared_ptr<http_stream> get_stream(boost::system::error_code& ec) const;

      private:
        net::strand<net::any_io_executor> strand_;
        tcp::resolver resolver_;
        std::string const host_;
        uint16_t const port_ = 0;
        scheme const scheme_ = scheme::plain;
        bool verify_ssl_ = true;
        std::string ca_cert_;

        std::atomic<std::shared_ptr<http_stream>> stream_;
        beast::flat_buffer buffer_;
    };

} // namespace httplib::client
