#pragma once
#include "httplib/client/ws_client.hpp"
#include "httplib/util/action_queue.hpp"
#include "stream/websocket_stream.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/result.hpp>
#include <spdlog/spdlog.h>

namespace httplib::client
{
    class ws_client::impl : public std::enable_shared_from_this<impl>
    {
      public:
        impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl);

      public:
        net::awaitable<boost::system::error_code> async_send(std::string&& data, bool binary = false);
        net::awaitable<boost::system::error_code> async_connect(std::string_view target,
                                                                http::fields const& headers = {});
        net::awaitable<boost::system::error_code> async_read();

        net::awaitable<boost::system::error_code> async_ping(std::string&& msg);
        net::awaitable<boost::system::error_code> async_pong(std::string&& msg);

        net::awaitable<boost::system::error_code> async_close();

        void send(std::string&& data, bool binary = false);
        void ping(std::string&& msg = std::string());
        void pong(std::string&& msg = std::string());
        void close();

        bool got_binary() const noexcept;
        bool got_text() const noexcept;
        std::string_view got_data() const noexcept;

        bool is_open() const noexcept;
        void abort();

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        void run(std::string_view target,
                 coro_open_handler_type&& open_handler,
                 coro_message_handler_type&& message_handler,
                 coro_close_handler_type&& close_handler,
                 http::fields const& headers = {});

        net::awaitable<boost::system::error_code> async_run(std::string_view target,
                                                            coro_message_handler_type&& message_handler,
                                                            coro_close_handler_type&& close_handler,
                                                            http::fields const& headers = {});

      private:
        net::awaitable<boost::system::error_code> _async_connect(std::string_view target,
                                                                 http::fields const& headers = {});
        net::awaitable<boost::system::error_code> _async_read();

      private:
        net::any_io_executor executor_;
        tcp::resolver resolver_;
        std::string host_;
        uint16_t port_ = 0;
        bool use_ssl_ = false;

        std::shared_ptr<websocket_stream> stream_;

        beast::flat_buffer buffer_;
        util::action_queue ac_que_;

        std::shared_ptr<spdlog::logger> default_logger_;
        std::shared_ptr<spdlog::logger> custom_logger_;
    };
} // namespace httplib::client