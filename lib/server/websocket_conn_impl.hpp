#pragma once
#include "httplib/action_queue.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/websocket_conn.hpp"
#include "httplib/use_awaitable.hpp"
#include "httplib/util/misc.hpp"
#include "server_impl.h"
#include "stream/websocket_stream.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <queue>
#include <span>

namespace httplib::server {

class websocket_conn_impl : public websocket_conn
{
public:
    websocket_conn_impl(http_server_impl& serv,
                        std::unique_ptr<websocket_stream>&& stream,
                        request&& req);
    ~websocket_conn_impl();

public:
    void send_message(std::string&& msg, bool binary) override;
    void send_ping(std::string&& msg) override;

    void close() override;
    const request& http_request() const override { return req_; }

public:
    net::awaitable<void> run();

private:
    http_server_impl& serv_;

    request req_;
    std::unique_ptr<websocket_stream> ws_;
    beast::flat_buffer buffer_;

    action_queue ac_que_;
};

} // namespace httplib::server