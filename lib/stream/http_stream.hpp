#pragma once

#pragma once
#ifdef HTTPLIB_ENABLED_SSL
#include "ssl_stream.hpp"
#endif
#include "boost/asio/use_awaitable.hpp"
#include "http_variant_stream.hpp"

namespace httplib {

// using http_stream =
//     beast::basic_stream<net::ip::tcp, net::any_io_executor, beast::simple_rate_policy>;
// #ifdef HTTPLIB_ENABLED_SSL
// using ssl_http_stream = ssl_stream<http_stream>;
//
// using http_variant_stream_type = http_variant_stream<http_stream, ssl_http_stream>;
// #else
// using http_variant_stream_type = http_variant_stream<http_stream>;
// #endif

class http_stream
{
public:
    using plain_stream =
        beast::basic_stream<net::ip::tcp, net::any_io_executor, beast::simple_rate_policy>;
#ifdef HTTPLIB_ENABLED_SSL
    using tls_stream = ssl_stream<plain_stream>;
    using stream_t   = std::variant<plain_stream, tls_stream>;
#else
    using stream_t = std::variant<plain_stream>;
#endif

    static std::unique_ptr<http_stream> create(const net::any_io_executor& executor,
                                               const std::string& host,
                                               bool use_ssl)
    {
        if (use_ssl) {
#ifdef HTTPLIB_ENABLED_SSL
            unsigned long ssl_options = ssl::context::default_workarounds | ssl::context::no_sslv2 |
                                        ssl::context::single_dh_use;

            auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
            ssl_ctx->set_options(ssl_options);
            ssl_ctx->set_default_verify_paths();
            ssl_ctx->set_verify_mode(ssl::verify_none);

            tls_stream stream(executor, ssl_ctx);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
                beast::error_code ec {static_cast<int>(::ERR_get_error()),
                                      net::error::get_ssl_category()};
                throw boost::system::system_error(ec);
            }
            return std::make_unique<http_stream>(std::move(stream));
#else
            throw boost::system::system_error(
                boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported));
#endif
        }
        else {
            return std::make_unique<http_stream>(plain_stream(executor));
        }
    }

public:
    using executor_type     = net::any_io_executor;
    using lowest_layer_type = tcp::socket::lowest_layer_type;

    executor_type get_executor()
    {
        return std::visit([&](auto& t) mutable { return t.get_executor(); }, stream_);
    }
    lowest_layer_type& lowest_layer()
    {
        return std::visit(
            [&](auto& t) mutable -> lowest_layer_type& {
                using stream_type = std::decay_t<decltype(beast::get_lowest_layer(t))>;
                if constexpr (is_basic_stream_v<stream_type>) {
                    return beast::get_lowest_layer(t).socket().lowest_layer();
                }
                else {
                    return t.lowest_layer();
                }
            },
            stream_);
    }
    const lowest_layer_type& lowest_layer() const
    {
        return std::visit(
            [&](auto& t) mutable -> const lowest_layer_type& {
                using stream_type = std::decay_t<decltype(beast::get_lowest_layer(t))>;
                if constexpr (is_basic_stream_v<stream_type>) {
                    return beast::get_lowest_layer(t).socket().lowest_layer();
                }
                else {
                    return t.lowest_layer();
                }
            },
            stream_);
    }
    template<typename MutableBufferSequence, typename ReadHandler>
    auto async_read_some(const MutableBufferSequence& buffers, ReadHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_read_some(buffers, std::forward<ReadHandler>(handler));
            },
            stream_);
    }
    template<typename ConstBufferSequence, typename WriteHandler>
    auto async_write_some(const ConstBufferSequence& buffers, WriteHandler&& handler)
    {
        return std::visit(
            [&, handler = std::move(handler)](auto& t) mutable {
                return t.async_write_some(buffers, std::forward<WriteHandler>(handler));
            },
            stream_);
    }

    tcp::endpoint remote_endpoint() { return lowest_layer().remote_endpoint(); }
    tcp::endpoint remote_endpoint(boost::system::error_code& ec)
    {
        return lowest_layer().remote_endpoint(ec);
    }

    tcp::endpoint local_endpoint() { return lowest_layer().local_endpoint(); }
    tcp::endpoint local_endpoint(boost::system::error_code& ec)
    {
        return lowest_layer().local_endpoint(ec);
    }

    void shutdown(net::socket_base::shutdown_type what, boost::system::error_code& ec)
    {
        lowest_layer().shutdown(what, ec);
    }

    bool is_open() const { return lowest_layer().is_open(); }

    void close(boost::system::error_code& ec) { lowest_layer().close(ec); }

    auto expires_after(const net::steady_timer::duration& expiry_time)
    {
        return std::visit(
            [&](auto& t) mutable { return beast::get_lowest_layer(t).expires_after(expiry_time); },
            stream_);
    }
    auto expires_never()
    {
        return std::visit(
            [&](auto& t) mutable { return beast::get_lowest_layer(t).expires_never(); }, stream_);
    }

    auto& rate_policy() & noexcept
    {
        return std::visit(
            [&](auto& t) mutable -> auto& { return beast::get_lowest_layer(t).rate_policy(); },
            stream_);
    }
    auto& rate_policy() const& noexcept
    {
        return std::visit(
            [&](auto& t) -> auto& { return beast::get_lowest_layer(t).rate_policy(); }, stream_);
    }

    void close()
    {
        std::visit(
            [](auto& stream) mutable {
                using stream_type = std::decay_t<decltype(stream)>;
#ifdef HTTPLIB_ENABLED_SSL
                if constexpr (std::is_same_v<stream_type, tls_stream>) {
                    boost::system::error_code ec;
                    stream.shutdown(ec);
                }
#endif
            },
            stream_);
        boost::system::error_code ec;
        shutdown(net::socket_base::shutdown_type::shutdown_both, ec);
        close(ec);
    }

    template<typename EndPoints>
    net::awaitable<void> async_connect(EndPoints&& endpoints)
    {
        co_await std::visit(
            [&](auto& stream) -> net::awaitable<void> {
                using stream_type = std::decay_t<decltype(stream)>;
#ifdef HTTPLIB_ENABLED_SSL
                if constexpr (std::is_same_v<stream_type, http_stream::tls_stream>) {
                    co_await stream.next_layer().async_connect(endpoints, net::use_awaitable);
                    co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);
                }
#endif
                if constexpr (std::is_same_v<stream_type, http_stream::plain_stream>) {
                    co_await stream.async_connect(endpoints, net::use_awaitable);
                }
            },
            stream_);

        net::socket_base::reuse_address option(true);
        lowest_layer().set_option(option);
        co_return;
    }

    auto&& release() { return std::move(stream_); }

    http_stream(stream_t&& stream)
        : stream_(std::move(stream))
    {
    }

private:
    stream_t stream_;
};

} // namespace httplib