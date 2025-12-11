#pragma once
#include "stream/http_stream.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>


namespace httplib::client::helper {

static http_variant_stream_type make_http_variant_stream(const net::any_io_executor& executor,
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

        ssl_http_stream ssl_stream(executor, ssl_ctx);
        if (!SSL_set_tlsext_host_name(ssl_stream.native_handle(), host.c_str())) {
            beast::error_code ec {static_cast<int>(::ERR_get_error()),
                                  net::error::get_ssl_category()};
            throw boost::system::system_error(ec);
        }
        return ssl_stream;
#else
        throw boost::system::system_error(
            boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported));
#endif
    }
    else {
        return http_stream(executor);
    }
}

template<typename EndPoints>
static net::awaitable<void> async_connect(http_variant_stream_type& variant_stream,
                                          EndPoints&& endpoints)
{
    co_await std::visit(
        [&](auto&& stream) -> net::awaitable<void> {
            using stream_type = std::decay_t<decltype(stream)>;
#ifdef HTTPLIB_ENABLED_SSL
            if constexpr (std::is_same_v<stream_type, ssl_http_stream>) {
                co_await stream.next_layer().async_connect(endpoints, net::use_awaitable);
                co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);
            }
#endif
            if constexpr (std::is_same_v<stream_type, http_stream>) {
                co_await stream.async_connect(endpoints, net::use_awaitable);
            }
        },
        variant_stream);

    net::socket_base::reuse_address option(true);
    variant_stream.lowest_layer().set_option(option);
    co_return;
}
} // namespace httplib::client::helper