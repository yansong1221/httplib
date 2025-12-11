#pragma once
#ifdef HTTPLIB_ENABLED_SSL
#include "ssl_stream.hpp"
#endif
#include "boost/asio/use_awaitable.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/basic_stream.hpp>

namespace httplib {

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
    using executor_type = plain_stream::executor_type;

    executor_type get_executor()
    {
        return std::visit([&](auto& t) mutable { return t.get_executor(); }, stream_);
    }
    auto& socket()
    {
        return std::visit(
            [&](auto& t) mutable -> auto& { return beast::get_lowest_layer(t).socket(); }, stream_);
    }
    const auto& socket() const
    {
        return std::visit(
            [&](auto& t) mutable -> const auto& { return beast::get_lowest_layer(t).socket(); },
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

    bool is_open() const { return socket().is_open(); }

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

    void shutdown(net::socket_base::shutdown_type what, boost::system::error_code& ec)
    {
        socket().shutdown(what, ec);
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
        socket().shutdown(net::socket_base::shutdown_type::shutdown_both, ec);
        socket().close(ec);
    }

    template<typename EndPoints>
    net::awaitable<void> async_connect(EndPoints&& endpoints)
    {
        co_await std::visit(
            [&](auto& t) -> net::awaitable<void> {
                using stream_type = std::decay_t<decltype(t)>;
#ifdef HTTPLIB_ENABLED_SSL
                if constexpr (std::is_same_v<stream_type, http_stream::tls_stream>) {
                    co_await t.next_layer().async_connect(endpoints, net::use_awaitable);
                    co_await t.async_handshake(ssl::stream_base::client, net::use_awaitable);
                }
#endif
                if constexpr (std::is_same_v<stream_type, http_stream::plain_stream>) {
                    co_await t.async_connect(endpoints, net::use_awaitable);
                }
            },
            stream_);

        net::socket_base::reuse_address option(true);
        socket().set_option(option);
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