#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <future>
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

        virtual std::future<boost::system::error_code> close(std::string_view reason) = 0;
        virtual net::awaitable<void> async_close(std::string_view reason, boost::system::error_code& ec) = 0;

        virtual std::future<void> abort() = 0;
        virtual net::awaitable<void> async_abort() = 0;

        virtual request const& http_request() const = 0;
        virtual request& http_request() = 0;

        virtual std::future<boost::system::error_code> send(std::string&& msg, bool binary) = 0;
        virtual net::awaitable<void> async_send(std::string_view msg, bool binary, boost::system::error_code& ec) = 0;

        virtual std::future<boost::system::error_code> ping(std::string&& msg = std::string()) = 0;
        virtual net::awaitable<void> async_ping(std::string_view msg, boost::system::error_code& ec) = 0;

        inline std::future<boost::system::error_code>
        send(std::string_view msg, bool binary)
        {
            return send(std::string(msg), binary);
        }

        inline std::future<boost::system::error_code>
        close()
        {
            using namespace std::string_view_literals;
            return close("normal"sv);
        }
        inline net::awaitable<void>
        async_close(boost::system::error_code& ec)
        {
            using namespace std::string_view_literals;
            co_return co_await async_close("normal"sv, ec);
        }
    };

} // namespace httplib::server