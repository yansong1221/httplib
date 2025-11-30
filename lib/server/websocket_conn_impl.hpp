#pragma once
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
                        websocket_variant_stream_type&& stream,
                        request&& req);

public:
    void send_message(std::string&& msg, data_type type) override;
    void close() override;
    const request& http_request() const override { return req_; }

public:
    net::awaitable<void> process_write_data();
    net::awaitable<void> run();

private:
    http_server_impl& serv_;

    request req_;
    net::strand<net::any_io_executor> strand_;
    websocket_variant_stream_type ws_;

    using message_type = std::pair<std::string, data_type>;
    std::queue<message_type> send_que_;
};

} // namespace httplib::server