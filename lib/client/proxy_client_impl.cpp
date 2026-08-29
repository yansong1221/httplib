#include "proxy_client_impl.h"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include "util/logging.hpp"
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <spdlog/spdlog.h>

namespace httplib::client
{

    proxy_client::impl::impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl)
        : executor_(ex)
        , resolver_(ex)
        , host_(host)
        , port_(port)
        , use_ssl_(ssl)
    {
        default_logger_ = httplib::detail::make_console_logger("httplib.proxy_client");
    }

    net::awaitable<boost::system::error_code>
    proxy_client::impl::async_connect(std::string_view target, http::fields const& headers)
    {
        logger()->trace("connecting proxy {}:{} -> {}", host_, port_, target);
        boost::system::error_code ec;

        if (is_open())
        {
            co_return boost::system::error_code {};
        }

        auto stream_result = http_stream::create_stream(executor_, host_, use_ssl_, verify_ssl_, ca_cert_);
        if (!stream_result)
        {
            logger()->error("proxy connect failed {}:{}: {}", host_, port_, stream_result.error().message());
            co_return stream_result.error();
        }
        auto stream = std::make_unique<http_stream>(std::move(*stream_result));

        auto endpoints = co_await resolver_.async_resolve(host_, std::to_string(port_), util::net_awaitable[ec]);
        if (ec)
        {
            logger()->error("proxy connect failed {}:{}: {}", host_, port_, ec.message());
            co_return ec;
        }

        ec = co_await stream->async_connect(endpoints);
        if (ec)
        {
            logger()->error("proxy connect failed {}:{}: {}", host_, port_, ec.message());
            co_return ec;
        }

        http::request<http::empty_body> req { http::verb::connect, target, 11 };
        req.set(http::field::host, target);
        for (auto const& h : headers)
        {
            req.set(h.name_string(), h.value());
        }

        http::request_serializer<http::empty_body> ser(req);
        co_await http::async_write(*stream, ser, util::net_awaitable[ec]);
        if (ec)
        {
            logger()->error("proxy write CONNECT failed {}:{}: {}", host_, port_, ec.message());
            co_return ec;
        }

        beast::flat_buffer resp_buf;
        http::response_parser<http::empty_body> parser;
        co_await http::async_read_header(*stream, resp_buf, parser, util::net_awaitable[ec]);
        if (ec)
        {
            logger()->error("proxy read CONNECT response failed {}:{}: {}", host_, port_, ec.message());
            co_return ec;
        }

        auto status = parser.get().result_int();
        if (status < 200 || status >= 300)
        {
            ec = http::error::bad_status;
            logger()->error("proxy CONNECT rejected {}:{}: status={}", host_, port_, status);
            co_return ec;
        }

        stream_ = std::move(stream);
        logger()->trace("proxy connected {}:{} -> {}", host_, port_, target);
        co_return boost::system::error_code {};
    }

    net::awaitable<boost::system::result<std::size_t>>
    proxy_client::impl::async_read_some(net::mutable_buffer const& buffer)
    {
        if (!is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }

        boost::system::error_code ec;
        auto n = co_await stream_->async_read_some(buffer, util::net_awaitable[ec]);
        if (ec)
        {
            co_return ec;
        }
        co_return n;
    }

    net::awaitable<boost::system::error_code>
    proxy_client::impl::async_write(net::const_buffer const& buffer)
    {
        if (!is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }

        boost::system::error_code ec;
        co_await stream_->async_write_some(buffer, util::net_awaitable[ec]);
        co_return ec;
    }

    void
    proxy_client::impl::close()
    {
        if (stream_)
        {
            stream_->close();
            stream_.reset();
        }
    }

    void
    proxy_client::impl::abort()
    {
        close();
    }

    bool
    proxy_client::impl::is_open() const noexcept
    {
        return stream_ && stream_->is_open();
    }

    std::shared_ptr<spdlog::logger>
    proxy_client::impl::logger() const
    {
        return custom_logger_ ? custom_logger_ : default_logger_;
    }

    void
    proxy_client::impl::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        custom_logger_ = std::move(logger);
    }

} // namespace httplib::client
