#pragma once
#include "httplib/request.hpp"
#include "httplib/server.hpp"
#include "httplib/use_awaitable.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/websocket_conn.hpp"
#include "stream/websocket_stream.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <queue>
#include <span>
#include <spdlog/spdlog.h>

namespace httplib {

class websocket_conn_impl : public websocket_conn {
public:
    websocket_conn_impl(const server::setting& option,
                        websocket_variant_stream_type&& stream)
        : option_(option), strand_(stream.get_executor()), ws_(std::move(stream)) { }
    void send_message(websocket_conn::message&& msg) override {
        if (!ws_.is_open()) return;

        net::post(strand_,
                  [this, msg = std::move(msg), self = shared_from_this()]() mutable {
                      bool send_in_process = !send_que_.empty();
                      send_que_.push(std::move(msg));
                      if (send_in_process) return;
                      net::co_spawn(strand_, process_write_data(), net::detached);
                  });
    }
    void close() override {
        if (!ws_.is_open()) return;

        net::co_spawn(
            strand_,
            [this, self = shared_from_this()]() -> net::awaitable<void> {
                boost::system::error_code ec;
                co_await net::post(strand_, net_awaitable[ec]);
                if (ec) co_return;

                websocket::close_reason reason("normal");
                co_await ws_.async_close(reason, net_awaitable[ec]);
            },
            net::detached);
    }

public:
    net::awaitable<void> process_write_data() {
        auto self = shared_from_this();

        for (;;) {
            boost::system::error_code ec;
            co_await net::post(strand_, net_awaitable[ec]);
            if (ec) co_return;
            if (send_que_.empty()) co_return;

            websocket_conn::message msg = std::move(send_que_.front());
            send_que_.pop();
            if (msg.type() == websocket_conn::message::data_type::text)
                ws_.text(true);
            else
                ws_.binary(true);
            co_await ws_.async_write(net::buffer(msg.payload()), net_awaitable[ec]);
            if (ec) co_return;
        }
    }
    net::awaitable<void> run(const request& req) {
        boost::system::error_code ec;
        auto remote_endp = ws_.remote_endpoint(ec);
        co_await ws_.async_accept(req, net_awaitable[ec]);
        if (ec) {
            option_.get_logger()->error("websocket handshake failed: {}", ec.message());
            co_return;
        }

        if (option_.websocket_open_handler)
            co_await option_.websocket_open_handler(weak_from_this());

        option_.get_logger()->debug("websocket new connection: [{}:{}]",
                                    remote_endp.address().to_string(),
                                    remote_endp.port());

        beast::flat_buffer buffer;
        for (;;) {
            auto bytes = co_await ws_.async_read(buffer, net_awaitable[ec]);
            if (ec) {
                option_.get_logger()->debug("websocket disconnect: [{}:{}] what: {}",
                                            remote_endp.address().to_string(),
                                            remote_endp.port(),
                                            ec.message());
                if (option_.websocket_close_handler)
                    co_await option_.websocket_close_handler(weak_from_this());
                co_return;
            }

            if (option_.websocket_message_handler) {
                websocket_conn::message msg(
                    util::buffer_to_string_view(buffer.data()),
                    ws_.got_text() ? websocket_conn::message::data_type::text
                                   : websocket_conn::message::data_type::binary);
                co_await option_.websocket_message_handler(weak_from_this(),
                                                           std::move(msg));
            }
            buffer.consume(bytes);
        }
    }

private:
    const server::setting& option_;
    net::strand<net::any_io_executor> strand_;
    websocket_variant_stream_type ws_;
    std::queue<websocket_conn::message> send_que_;
};

} // namespace httplib