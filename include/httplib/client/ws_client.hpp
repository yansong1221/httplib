#pragma once
#include "httplib/config.hpp"
#include "httplib/server/helper.hpp"
#include "httplib/util/misc.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/system/result.hpp>
#include <string_view>

namespace httplib::client {
class ws_client
{
public:
    explicit ws_client(net::io_context& ex, std::string_view host, uint16_t port, bool ssl = false);
    explicit ws_client(const net::any_io_executor& ex,
                       std::string_view host,
                       uint16_t port,
                       bool ssl = false);
    ~ws_client();

public:
    net::awaitable<boost::system::error_code> async_read();
    net::awaitable<boost::system::error_code> async_ping(std::string&& msg);
    net::awaitable<boost::system::error_code> async_pong(std::string&& msg);
    net::awaitable<boost::system::error_code> async_close();
    net::awaitable<boost::system::error_code> async_send(std::string&& data, bool binary = false);

    net::awaitable<boost::system::error_code> async_connect(std::string_view path,
                                                            const http::fields& headers = {});


    bool got_binary() const noexcept;
    bool got_text() const noexcept;
    std::string_view got_data() const noexcept;

    void async_run(std::string_view path, const http::fields& headers = {});

    void send(std::string&& data, bool binary = false);
    void close();

    template<typename OpenFunc, typename MessageFunc, typename CloseFunc>
    void set_handler(OpenFunc&& open_handler,
                     MessageFunc&& message_handler,
                     CloseFunc&& close_handler)
    {
        set_handler_impl(
            httplib::util::make_coro_handler(std::forward<OpenFunc>(open_handler)),
            httplib::util::make_coro_handler(std::forward<MessageFunc>(message_handler)),
            httplib::util::make_coro_handler(std::forward<CloseFunc>(close_handler)));
    }

private:
    using coro_open_handler_type  = std::function<net::awaitable<void>(boost::system::error_code)>;
    using coro_close_handler_type = std::function<net::awaitable<void>()>;
    using coro_message_handler_type =
        std::function<net::awaitable<void>(std::string_view, bool binary)>;


    void set_handler_impl(coro_open_handler_type&& open_handler,
                          coro_message_handler_type&& message_handler,
                          coro_close_handler_type&& close_handler);

private:
    ws_client(const ws_client&)            = delete;
    ws_client& operator=(const ws_client&) = delete;

    class impl;
    std::shared_ptr<impl> impl_;
};
} // namespace httplib::client