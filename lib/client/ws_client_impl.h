#pragma once
#include "httplib/client/ws_client.hpp"
#include "httplib/util/action_queue.hpp"
#include "stream/websocket_stream.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/result.hpp>

namespace httplib::client {
class ws_client::impl : public std::enable_shared_from_this<impl>
{
public:
    impl(const net::any_io_executor& ex, std::string_view host, uint16_t port, bool ssl);

public:
    net::awaitable<boost::system::error_code> async_connect(std::string_view path,
                                                            const http::fields& headers = {});
    net::awaitable<boost::system::error_code> async_send(std::string&& data, bool binary = false);

    net::awaitable<boost::system::error_code> async_read();

    net::awaitable<boost::system::error_code> async_ping(std::string&& msg);
    net::awaitable<boost::system::error_code> async_pong(std::string&& msg);

    net::awaitable<boost::system::error_code> async_close();

    void async_run(std::string_view path, const http::fields& headers = {});

    void send(std::string&& data, bool binary = false);
    void close();

    bool got_binary() const noexcept;
    bool got_text() const noexcept;
    std::string_view got_data() const noexcept;

    bool is_open() const;


    void set_handler_impl(coro_open_handler_type&& open_handler,
                          coro_message_handler_type&& message_handler,
                          coro_close_handler_type&& close_handler);

private:
    net::any_io_executor executor_;
    tcp::resolver resolver_;
    std::string host_;
    uint16_t port_ = 0;
    bool use_ssl_  = false;

    std::shared_ptr<websocket_stream> stream_;

    beast::flat_buffer buffer_;
    util::action_queue ac_que_;

    ws_client::coro_open_handler_type open_handler_;
    ws_client::coro_message_handler_type message_handler_;
    ws_client::coro_close_handler_type close_handler_;
};
} // namespace httplib::client