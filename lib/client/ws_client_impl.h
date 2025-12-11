#pragma once
#include "httplib/client/ws_client.hpp"
#include "stream/websocket_stream.hpp"
#include <boost/asio/ip/tcp.hpp>

namespace httplib::client {
class ws_client::impl
{
public:
    impl(const net::any_io_executor& ex, std::string_view host, uint16_t port, bool ssl);

public:
    net::awaitable<boost::system::error_code> async_connect(std::string_view path,
                                                            const http::fields& headers = {});

    bool got_binary() const noexcept;
    bool got_text() const noexcept;

    bool is_open() const;

private:
    net::any_io_executor executor_;
    tcp::resolver resolver_;
    std::string host_;
    uint16_t port_ = 0;
    bool use_ssl_  = false;

    mutable std::recursive_mutex stream_mutex_;
    std::unique_ptr<websocket_stream> stream_;
};
} // namespace httplib::client