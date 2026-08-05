#pragma once
#include "httplib/server/request.hpp"
#include "httplib/server/websocket_conn.hpp"
#include "httplib/util/action_queue.hpp"
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
        void send(std::string&& msg, bool binary) override;
        void ping(std::string&& msg) override;
        void close(std::string_view reason) override;
        bool is_open() const override;

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
        std::shared_ptr<http_server::impl> server_impl_;

        request req_;
        websocket_stream ws_;
        beast::flat_buffer buffer_;

        util::action_queue ac_que_;
        std::atomic_bool shutting_down_ { false };
    };

} // namespace httplib::server