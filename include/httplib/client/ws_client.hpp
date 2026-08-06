#pragma once
#include "httplib/config.hpp"
#include "httplib/util/misc.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/system/result.hpp>
#include <string_view>

namespace httplib::client
{
    class HTTPLIB_API ws_client
    {
      public:
        explicit ws_client(net::io_context& ex, std::string_view host, uint16_t port, bool ssl = false);
        explicit ws_client(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl = false);
        ~ws_client();

      public:
        net::awaitable<boost::system::error_code> async_read();
        net::awaitable<boost::system::error_code> async_ping(std::string&& msg);
        net::awaitable<boost::system::error_code> async_pong(std::string&& msg);
        net::awaitable<boost::system::error_code> async_close();
        net::awaitable<boost::system::error_code> async_send(std::string&& data, bool binary = false);

        net::awaitable<boost::system::error_code> async_connect(std::string_view target,
                                                                http::fields const& headers = {});

        bool got_binary() const noexcept;
        bool got_text() const noexcept;
        std::string_view got_data() const noexcept;

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        template <typename OpenFunc, typename MessageFunc, typename CloseFunc>
        void
        run(std::string_view target,
            OpenFunc&& open_handler,
            MessageFunc&& message_handler,
            CloseFunc&& close_handler,
            http::fields const& headers = {})
        {
            run_impl(target,
                     httplib::util::make_coro_handler(std::forward<OpenFunc>(open_handler)),
                     httplib::util::make_coro_handler(std::forward<MessageFunc>(message_handler)),
                     httplib::util::make_coro_handler(std::forward<CloseFunc>(close_handler)),
                     headers);
        }

        template <typename MessageFunc, typename CloseFunc>
        net::awaitable<boost::system::error_code>
        async_run(std::string_view target,
                  MessageFunc&& message_handler,
                  CloseFunc&& close_handler,
                  http::fields const& headers = {})
        {
            co_return co_await async_run_impl(
                target,
                httplib::util::make_coro_handler(std::forward<MessageFunc>(message_handler)),
                httplib::util::make_coro_handler(std::forward<CloseFunc>(close_handler)),
                headers);
        }

        void send(std::string&& data, bool binary = false);
        void ping(std::string&& msg = std::string());
        void pong(std::string&& msg = std::string());
        void close();

      private:
        using coro_open_handler_type = std::function<net::awaitable<void>(boost::system::error_code)>;
        using coro_close_handler_type = std::function<net::awaitable<void>()>;
        using coro_message_handler_type = std::function<net::awaitable<void>(std::string_view, bool binary)>;

        net::awaitable<boost::system::error_code> async_run_impl(std::string_view target,
                                                                 coro_message_handler_type&& message_handler,
                                                                 coro_close_handler_type&& close_handler,
                                                                 http::fields const& headers = {});
        void run_impl(std::string_view target,
                      coro_open_handler_type&& open_handler,
                      coro_message_handler_type&& message_handler,
                      coro_close_handler_type&& close_handler,
                      http::fields const& headers = {});

      private:
        ws_client(ws_client const&) = delete;
        ws_client& operator=(ws_client const&) = delete;

        class impl;
        std::shared_ptr<impl> impl_;
    };
} // namespace httplib::client