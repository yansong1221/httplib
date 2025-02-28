#pragma once
#include "httplib/use_awaitable.hpp"
#include "httplib/util/misc.hpp"
#include "request.hpp"
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <queue>
#include <span>
#include <spdlog/spdlog.h>

namespace httplib
{
class websocket_conn : public std::enable_shared_from_this<websocket_conn>
{
public:
    class message
    {
    public:
        enum class data_type
        {
            text,
            binary
        };

    public:
        explicit message(std::string&& payload, data_type type = data_type::text)
            : payload_(std::move(payload)), type_(type) { };
        explicit message(std::string_view payload, data_type type = data_type::text)
            : payload_(payload), type_(type) { };
        message(const message&) = default;
        message(message&&) = default;
        message& operator=(message&&) = default;
        message& operator=(const message&) = default;

    public:
        const std::string& payload() const { return payload_; }
        data_type type() const { return type_; }

    private:
        std::string payload_;
        data_type type_;
    };

    using weak_ptr = std::weak_ptr<websocket_conn>;

    using open_handler_type = std::function<net::awaitable<void>(websocket_conn::weak_ptr)>;
    using close_handler_type = std::function<net::awaitable<void>(websocket_conn::weak_ptr)>;
    using message_handler_type = std::function<net::awaitable<void>(websocket_conn::weak_ptr, message)>;

public:
    virtual ~websocket_conn() = default;

    virtual void set_message_handler(message_handler_type handler) = 0;
    virtual void set_open_handler(open_handler_type handler) = 0;
    virtual void set_close_handler(close_handler_type handler) = 0;
    virtual void send_message(message&& msg) = 0;
    virtual void close() = 0;
    virtual net::awaitable<void> run(const request& req) = 0;

    void send_message(const message& msg) { send_message(message(msg)); }
};

} // namespace httplib