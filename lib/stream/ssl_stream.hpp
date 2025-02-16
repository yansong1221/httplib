#pragma once
#include <boost/beast/core/detail/config.hpp>

// This include is necessary to work with `ssl::stream` and `boost::beast::websocket::stream`
#include <boost/beast/websocket/ssl.hpp>

// VFALCO We include this because anyone who uses ssl will
//        very likely need to check for ssl::error::stream_truncated
#include <boost/asio/ssl/error.hpp>

#include <boost/asio/ssl/stream.hpp>
#include <memory>

namespace httplib::stream {
namespace net = boost::asio;
namespace beast = boost::beast;
template<class NextLayer>
struct ssl_stream : public net::ssl::stream<NextLayer> {
    using net::ssl::stream<NextLayer>::stream;

    template<typename Arg>
    ssl_stream(Arg &&arg, std::shared_ptr<net::ssl::context> ctx)
        : net::ssl::stream<NextLayer>(std::move(arg), *ctx), ssl_ctx_(ctx) {}

private:
    std::shared_ptr<net::ssl::context> ssl_ctx_;
};

template<class SyncStream>
void teardown(boost::beast::role_type role, ssl_stream<SyncStream> &stream,
              boost::system::error_code &ec) {
    // Just forward it to the underlying ssl::stream
    using boost::beast::websocket::teardown;
    teardown(role, static_cast<net::ssl::stream<SyncStream> &>(stream), ec);
}

template<class AsyncStream, typename TeardownHandler =
                                net::default_completion_token_t<beast::executor_type<AsyncStream>>>
void async_teardown(boost::beast::role_type role, ssl_stream<AsyncStream> &stream,
                    TeardownHandler &&handler =
                        net::default_completion_token_t<beast::executor_type<AsyncStream>>{}) {
    // Just forward it to the underlying ssl::stream
    using boost::beast::websocket::async_teardown;
    async_teardown(role, static_cast<net::ssl::stream<AsyncStream> &>(stream),
                   std::forward<TeardownHandler>(handler));
}
} // namespace httplib::stream