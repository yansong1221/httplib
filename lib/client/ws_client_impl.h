#pragma once
#include "httplib/client/ws_client.hpp"
#include "httplib/util/async_mutex.hpp"
#include "stream/websocket_stream.hpp"
#include "util/logging.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <future>

namespace httplib::client
{
    class ws_client::impl
        : public httplib::detail::logger
        , public std::enable_shared_from_this<impl>
    {
      public:
        impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, scheme s);

      public:
        net::awaitable<void> async_send(websocket_message const& msg, boost::system::error_code& ec);
        std::future<boost::system::error_code> send(websocket_message msg);

        net::awaitable<void> async_connect(std::string_view target,
                                           http::fields const& headers,
                                           std::chrono::steady_clock::duration timeout,
                                           boost::system::error_code& ec);

        net::awaitable<void> async_read(websocket_message& msg, boost::system::error_code& ec);

        net::awaitable<void> async_ping(std::string_view msg, boost::system::error_code& ec);
        std::future<boost::system::error_code> ping(std::string&& msg = std::string());

        net::awaitable<void> async_pong(std::string_view msg, boost::system::error_code& ec);
        std::future<boost::system::error_code> pong(std::string&& msg = std::string());

        net::awaitable<void> async_close(boost::system::error_code& ec);
        std::future<boost::system::error_code> close();

        bool is_open() const noexcept;
        std::future<void> abort();
        net::awaitable<void> async_abort();

        void
        set_verify_ssl(bool verify)
        {
            verify_ssl_.store(verify);
        }
        void
        set_ca_cert(std::string_view cert)
        {
            ca_cert_.store(std::make_shared<std::string const>(cert));
        }

        void run(std::string_view target,
                 coro_open_handler_type&& open_handler,
                 coro_message_handler_type&& message_handler,
                 coro_close_handler_type&& close_handler,
                 http::fields const& headers = {});

        net::awaitable<void> async_run(std::string_view target,
                                       http::fields const& headers,
                                       coro_message_handler_type&& message_handler,
                                       coro_close_handler_type&& close_handler,
                                       boost::system::error_code& ec);

      private:
        std::shared_ptr<websocket_stream> get_stream(boost::system::error_code& ec) const;

      private:
        net::strand<net::any_io_executor> strand_;
        std::string const host_;
        uint16_t const port_ = 0;
        scheme const scheme_ = scheme::plain;
        std::atomic<bool> verify_ssl_ = true;
        std::atomic<std::shared_ptr<std::string const>> ca_cert_;

        std::atomic<std::shared_ptr<websocket_stream>> stream_;

        util::async_mutex write_mutex_;
        util::async_mutex read_mutex_;
    };
} // namespace httplib::client