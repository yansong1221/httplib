#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <memory>
#include <string_view>

namespace httplib::server
{
    class HTTPLIB_API websocket_conn : public std::enable_shared_from_this<websocket_conn>
    {
      public:
        using weak_ptr = std::weak_ptr<websocket_conn>;

        using coro_open_handler_type = std::function<net::awaitable<void>(websocket_conn::weak_ptr)>;
        using coro_close_handler_type = coro_open_handler_type;
        using coro_message_handler_type
            = std::function<net::awaitable<void>(websocket_conn::weak_ptr, std::string_view, bool binary)>;

      public:
        virtual ~websocket_conn() = default;

        virtual void close(std::string_view reason) = 0;
        virtual bool is_open() const = 0;
        virtual request const& http_request() const = 0;
        virtual void send(std::string&& msg, bool binary = true) = 0;
        virtual void ping(std::string&& msg = std::string()) = 0;

        inline void
        send(std::string_view msg, bool binary = true)
        {
            return send(std::string(msg), binary);
        }
        inline void
        close()
        {
            using namespace std::string_view_literals;
            close("normal"sv);
        }
    };

} // namespace httplib::server