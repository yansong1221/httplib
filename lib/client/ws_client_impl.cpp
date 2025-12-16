#include "ws_client_impl.h"
#include "helper.hpp"

namespace httplib::client {
ws_client::impl::impl(const net::any_io_executor& ex,
                      std::string_view host,
                      uint16_t port,
                      bool ssl)
    : executor_(ex)
    , resolver_(ex)
    , host_(host)
    , port_(port)
    , use_ssl_(ssl)
{
}

net::awaitable<boost::system::error_code>
ws_client::impl::async_connect(std::string_view path, const http::fields& headers)
{
    try {
        // Set up an HTTP GET request message
        if (!is_open()) {
            std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
            http_stream stream(executor_, host_, use_ssl_);
            auto endpoints =
                co_await resolver_.async_resolve(host_, std::to_string(port_), net::use_awaitable);

            co_await stream.async_connect(endpoints);
            stream_ = std::make_unique<websocket_stream>(std::move(stream));
        }
        stream_->set_option(websocket::stream_base::decorator([&](websocket::request_type& req) {
            req.set(http::field::user_agent,
                    std::string(BOOST_BEAST_VERSION_STRING) + "websocket-client-coro");
            for (const auto& field : headers)
                req.set(field.name_string(), field.value());
        }));

        co_await stream_->async_handshake(host_, path, net::use_awaitable);

        co_return boost::system::error_code {};
    }
    catch (const boost::system::system_error& e) {
        co_return e.code();
    }
}

bool ws_client::impl::is_open() const
{
    std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
    return stream_ && stream_->is_open();
}

bool ws_client::impl::got_binary() const noexcept
{
    std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
    return stream_ && stream_->got_binary();
}

bool ws_client::impl::got_text() const noexcept
{
    std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
    return stream_ && stream_->got_text();
}

} // namespace httplib::client
