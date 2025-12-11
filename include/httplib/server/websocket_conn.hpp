#pragma once
#include "httplib/server/request.hpp"
#include "httplib/use_awaitable.hpp"
#include "httplib/util/misc.hpp"
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <queue>
#include <span>
#include <spdlog/spdlog.h>

namespace httplib::server {
class websocket_conn : public std::enable_shared_from_this<websocket_conn>
{
public:
    using weak_ptr = std::weak_ptr<websocket_conn>;

    using coro_open_handler_type    = std::function<net::awaitable<void>(websocket_conn::weak_ptr)>;
    using coro_close_handler_type   = coro_open_handler_type;
    using coro_message_handler_type = std::function<net::awaitable<void>(
        websocket_conn::weak_ptr, std::string_view, bool binary)>;

public:
    virtual ~websocket_conn() = default;

    virtual void close()                                             = 0;
    virtual const request& http_request() const                      = 0;
    virtual void send_message(std::string&& msg, bool binary = true) = 0;
    virtual void send_ping(std::string&& msg = std::string())        = 0;

    inline void send_message(std::string_view msg, bool binary = true)
    {
        return send_message(std::string(msg), binary);
    };
};

} // namespace httplib::server