#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <memory>
#include <variant>

namespace httplib {

class websocket_conn;
class websocket_message;

using websocket_conn_ptr    = std::weak_ptr<websocket_conn>;
using websocket_message_ptr = std::shared_ptr<websocket_message>;

using coro_websocket_open_handler_type  = std::function<net::awaitable<void>(websocket_conn_ptr)>;
using coro_websocket_close_handler_type = std::function<net::awaitable<void>(websocket_conn_ptr)>;
using coro_websocket_message_handler_type =
    std::function<net::awaitable<void>(websocket_conn_ptr, websocket_message_ptr)>;

using websocket_open_handler_type  = std::function<void(websocket_conn_ptr)>;
using websocket_close_handler_type = std::function<void(websocket_conn_ptr)>;
using websocket_message_handler_type =
    std::function<void(websocket_conn_ptr, websocket_message_ptr)>;

} // namespace httplib