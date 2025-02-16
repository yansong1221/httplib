#include "httplib/websocket_conn.hpp"
#include "httplib/use_awaitable.hpp"
#include "httplib/utils.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <spdlog/spdlog.h>

namespace httplib {

websocket_conn::websocket_conn(std::shared_ptr<spdlog::logger> logger,
                               stream::http_stream_variant_type &&stream)
    : logger_(logger), strand_(stream.get_executor()) {
    std::visit(
        [this](auto &&t) {
            using value_type = std::decay_t<decltype(t)>;
            if constexpr (std::same_as<stream::http_stream, value_type>) {
                ws_ = std::make_unique<stream::ws_stream_variant_type>(
                    stream::ws_stream(std::move(t)));
            } else if constexpr (std::same_as<stream::ssl_http_stream, value_type>) {
                ws_ = std::make_unique<stream::ws_stream_variant_type>(
                    stream::ssl_ws_stream(std::move(t)));
            } else {
                static_assert(false, "unknown http_variant_stream_type");
            }
        },
        stream);
}
void websocket_conn::send_message(message &&msg) {
    if (!ws_->is_open())
        return;

    net::post(strand_, [this, msg = std::move(msg), self = shared_from_this()]() mutable {
        bool send_in_process = !send_que_.empty();
        send_que_.push(std::move(msg));
        if (send_in_process)
            return;
        net::co_spawn(strand_, process_write_data(), net::detached);
    });
}

void websocket_conn::close() {
    if (!ws_->is_open())
        return;

    net::co_spawn(
        strand_,
        [this, self = shared_from_this()]() -> net::awaitable<void> {
            boost::system::error_code ec;
            co_await net::post(strand_, net_awaitable[ec]);
            if (ec)
                co_return;

            websocket::close_reason reason("normal");
            co_await ws_->async_close(reason, net_awaitable[ec]);
        },
        net::detached);
}

net::awaitable<void> websocket_conn::process_write_data() {
    auto self = shared_from_this();

    for (;;) {
        boost::system::error_code ec;
        co_await net::post(strand_, net_awaitable[ec]);
        if (ec)
            co_return;
        if (send_que_.empty())
            co_return;

        message msg = std::move(send_que_.front());
        send_que_.pop();
        if (msg.type() == message::data_type::text)
            ws_->text(true);
        else
            ws_->binary(true);
        co_await ws_->async_write(net::buffer(msg.payload()), net_awaitable[ec]);
        if (ec)
            co_return;
    }
}

net::awaitable<void> websocket_conn::run(const http::request<http::empty_body> &req) {
    boost::system::error_code ec;
    auto remote_endp = ws_->remote_endpoint(ec);
    co_await ws_->async_accept(req, net_awaitable[ec]);
    if (ec) {
        logger_->error("websocket handshake failed: {}", ec.message());
        co_return;
    }

    if (open_handler_)
        co_await open_handler_(weak_from_this());

    logger_->debug("websocket new connection: [{}:{}]", remote_endp.address().to_string(),
                   remote_endp.port());

    beast::flat_buffer buffer;
    for (;;) {
        auto bytes = co_await ws_->async_read(buffer, net_awaitable[ec]);
        if (ec) {
            logger_->debug("websocket disconnect: [{}:{}] what: {}",
                           remote_endp.address().to_string(), remote_endp.port(), ec.message());
            if (close_handler_)
                co_await close_handler_(weak_from_this());
            co_return;
        }

        if (message_handler_) {
            message msg(utils::buffer_to_string_view(buffer.data()),
                        ws_->got_text() ? message::data_type::text : message::data_type::binary);
            net::co_spawn(co_await net::this_coro::executor,
                          message_handler_(weak_from_this(), std::move(msg)), net::detached);
        }
        buffer.consume(bytes);
    }
}

} // namespace httplib