#include "proxy_client_impl.h"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include "util/logging.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <spdlog/spdlog.h>

namespace httplib::client
{

    proxy_client::impl::impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, scheme s)
        : strand_(net::make_strand(ex))
        , resolver_(strand_)
        , host_(host)
        , port_(port)
        , scheme_(s)
        , httplib::detail::logger("httplib.proxy_client")
    {
    }

    net::awaitable<void>
    proxy_client::impl::async_connect(std::string_view target,
                                      http::fields const& headers,
                                      boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            strand_,
            [&]() -> net::awaitable<void>
            {
                if (is_open())
                {
                    ec = boost::system::error_code {};
                    co_return;
                }
                get_logger()->trace("connecting proxy {}:{} -> {}", host_, port_, target);

                auto stream_result = http_stream::create(strand_, host_, scheme_ == scheme::tls, verify_ssl_, ca_cert_);
                if (!stream_result)
                {
                    ec = stream_result.error();
                    get_logger()->error("proxy connect failed {}:{}: {}", host_, port_, ec.message());
                    co_return;
                }
                auto stream = std::make_shared<http_stream>(std::move(*stream_result));

                auto endpoints
                    = co_await resolver_.async_resolve(host_, std::to_string(port_), util::net_awaitable[ec]);
                if (ec)
                {
                    get_logger()->error("proxy connect failed {}:{}: {}", host_, port_, ec.message());
                    co_return;
                }

                co_await stream->async_connect(endpoints, ec);
                if (ec)
                {
                    get_logger()->error("proxy connect failed {}:{}: {}", host_, port_, ec.message());
                    co_return;
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
                    get_logger()->error("proxy write CONNECT failed {}:{}: {}", host_, port_, ec.message());
                    co_return;
                }

                beast::flat_buffer resp_buf;
                http::response_parser<http::empty_body> parser;
                co_await http::async_read_header(*stream, resp_buf, parser, util::net_awaitable[ec]);
                if (ec)
                {
                    get_logger()->error("proxy read CONNECT response failed {}:{}: {}", host_, port_, ec.message());
                    co_return;
                }

                auto status = parser.get().result_int();
                if (status < 200 || status >= 300)
                {
                    ec = http::error::bad_status;
                    get_logger()->error("proxy CONNECT rejected {}:{}: status={}", host_, port_, status);
                    co_return;
                }

                stream_.store(stream);
                get_logger()->trace("proxy connected {}:{} -> {}", host_, port_, target);
                ec = {};
            },
            net::use_awaitable);
    }

    net::awaitable<std::size_t>
    proxy_client::impl::async_read_some(net::mutable_buffer const& buffer, boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            strand_,
            [&]() -> net::awaitable<std::size_t>
            {
                auto s = get_stream(ec);
                if (ec)
                {
                    co_return 0;
                }
                co_return co_await s->async_read_some(buffer, util::net_awaitable[ec]);
                if (ec)
                {
                    co_await async_close();
                }
            },
            net::use_awaitable);
    }

    net::awaitable<void>
    proxy_client::impl::async_write(net::const_buffer const& buffer, boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            strand_,
            [&]() -> net::awaitable<void>
            {
                auto s = get_stream(ec);
                if (ec)
                {
                    co_return;
                }
                co_await net::async_write(*s, buffer, util::net_awaitable[ec]);
                if (ec)
                {
                    co_await async_close();
                }
            },
            net::use_awaitable);
    }

    std::future<void>
    proxy_client::impl::close()
    {
        return net::co_spawn(
            strand_,
            [this, self = shared_from_this()]() -> net::awaitable<void>
            {
                co_await async_close();
                co_return;
            },
            net::use_future);
    }

    bool
    proxy_client::impl::is_open() const noexcept
    {
        auto s = stream_.load();
        return s && s->is_open();
    }

    net::awaitable<void>
    proxy_client::impl::async_close()
    {
        co_await net::dispatch(strand_, net::use_awaitable);
        if (auto s = stream_.exchange(nullptr); s)
        {
            boost::system::error_code ec;
            s->close(ec);
        }
    }

    std::shared_ptr<httplib::http_stream>
    proxy_client::impl::get_stream(boost::system::error_code& ec) const
    {
        if (auto s = stream_.load(); s && s->is_open())
        {
            ec = {};
            return s;
        }
        ec = boost::system::errc::make_error_code(boost::system::errc::not_connected);
        return nullptr;
    }

    proxy_client::proxy_client(net::io_context& ex, std::string_view host, uint16_t port, scheme s /*= scheme::plain*/)
        : proxy_client(ex.get_executor(), host, port, s)
    {
    }

    proxy_client::proxy_client(net::any_io_executor const& ex,
                               std::string_view host,
                               uint16_t port,
                               scheme s /*= scheme::plain*/)
        : impl_(std::make_shared<proxy_client::impl>(ex, host, port, s))
    {
    }

    proxy_client::~proxy_client()
    {
        if (impl_)
        {
            impl_->close();
        }
    }

    net::awaitable<void>
    proxy_client::async_connect(std::string_view target, http::fields const& headers, boost::system::error_code& ec)
    {
        co_return co_await impl_->async_connect(target, headers, ec);
    }

    net::awaitable<std::size_t>
    proxy_client::async_read_some(net::mutable_buffer const& buffer, boost::system::error_code& ec)
    {
        co_return co_await impl_->async_read_some(buffer, ec);
    }

    net::awaitable<void>
    proxy_client::async_write(net::const_buffer const& buffer, boost::system::error_code& ec)
    {
        co_return co_await impl_->async_write(buffer, ec);
    }

    std::future<void>
    proxy_client::close()
    {
        return impl_->close();
    }

    bool
    proxy_client::is_open() const noexcept
    {
        return impl_->is_open();
    }

    std::shared_ptr<spdlog::logger>
    proxy_client::logger() const
    {
        return impl_->get_logger();
    }

    void
    proxy_client::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        impl_->set_logger(std::move(logger));
    }

    void
    proxy_client::set_verify_ssl(bool verify)
    {
        impl_->set_verify_ssl(verify);
    }

    void
    proxy_client::set_ca_cert(std::string_view cert)
    {
        impl_->set_ca_cert(cert);
    }

    net::awaitable<void>
    proxy_client::async_close()
    {
        co_return co_await impl_->async_close();
    }

} // namespace httplib::client
