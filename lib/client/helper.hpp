#pragma once
#include "stream/http_stream.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>


namespace httplib::client::helper {


//template<typename EndPoints>
//static net::awaitable<void> async_connect(http_stream& variant_stream, EndPoints&& endpoints)
//{
//    co_await std::visit(
//        [&](auto&& stream) -> net::awaitable<void> {
//            using stream_type = std::decay_t<decltype(stream)>;
//#ifdef HTTPLIB_ENABLED_SSL
//            if constexpr (std::is_same_v<stream_type, http_stream::tls_stream>) {
//                co_await stream.next_layer().async_connect(endpoints, net::use_awaitable);
//                co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);
//            }
//#endif
//            if constexpr (std::is_same_v<stream_type, http_stream::plain_stream>) {
//                co_await stream.async_connect(endpoints, net::use_awaitable);
//            }
//        },
//        variant_stream);
//
//    net::socket_base::reuse_address option(true);
//    variant_stream.lowest_layer().set_option(option);
//    co_return;
//}
} // namespace httplib::client::helper