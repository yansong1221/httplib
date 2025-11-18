#pragma once
#include "httplib/use_awaitable.hpp"
#include "httplib/util/misc.hpp"
#include "request.hpp"
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <queue>
#include <span>
#include <spdlog/spdlog.h>

namespace httplib {
class websocket_conn : public std::enable_shared_from_this<websocket_conn>
{
public:
    enum class data_type
    {
        text,
        binary
    };

    using weak_ptr = std::weak_ptr<websocket_conn>;

    using open_handler_type  = std::function<void(websocket_conn::weak_ptr)>;
    using close_handler_type = std::function<void(websocket_conn::weak_ptr)>;
    using message_handler_type =
        std::function<void(websocket_conn::weak_ptr, std::string_view, data_type)>;

    using coro_open_handler_type  = std::function<net::awaitable<void>(websocket_conn::weak_ptr)>;
    using coro_close_handler_type = coro_open_handler_type;
    using coro_message_handler_type =
        std::function<net::awaitable<void>(websocket_conn::weak_ptr, std::string_view, data_type)>;

public:
    virtual ~websocket_conn() = default;

    virtual void close()                                                           = 0;
    virtual const request& http_request() const                                    = 0;
    virtual void send_message(std::string&& msg, data_type type = data_type::text) = 0;

    void send_message(std::string_view msg, data_type type = data_type::text)
    {
        return send_message(std::string(msg), type);
    };
};

} // namespace httplib