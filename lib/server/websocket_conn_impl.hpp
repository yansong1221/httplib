#pragma once
#include "httplib/server/request.hpp"
#include "httplib/server/websocket_conn.hpp"
#include "httplib/util/action_queue.hpp"
#include "httplib/util/async_mutex.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "server_impl.h"
#include "stream/websocket_stream.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <queue>
#include <span>

namespace httplib::server
{

    class websocket_conn_impl : public websocket_conn
    {
      public:
        websocket_conn_impl(std::shared_ptr<http_server::impl> server_impl, websocket_stream&& stream, request&& req);
        ~websocket_conn_impl();

      public:
        std::future<boost::system::error_code> send(websocket_message msg) override;
        net::awaitable<void> async_send(websocket_message const& msg, boost::system::error_code& ec) override;

        std::future<boost::system::error_code> ping(std::string&& msg) override;
        net::awaitable<void> async_ping(std::string_view msg, boost::system::error_code& ec) override;

        std::future<boost::system::error_code> close(std::string_view reason) override;
        net::awaitable<void> async_close(std::string_view reason, boost::system::error_code& ec) override;

        std::future<void> abort() override;
        net::awaitable<void> async_abort() override;

        request const&
        http_request() const override
        {
            return req_;
        }
        request&
        http_request() override
        {
            return req_;
        }

      public:
        net::awaitable<void> run();

      private:
        std::shared_ptr<spdlog::logger> get_logger() const;

        std::shared_ptr<http_server::impl> server_impl_;

        request req_;
        websocket_stream ws_;
        util::async_mutex write_mutex_;
    };

} // namespace httplib::server