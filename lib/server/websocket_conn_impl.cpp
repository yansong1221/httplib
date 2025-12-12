#include "websocket_conn_impl.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <spdlog/spdlog.h>

namespace httplib::server {

websocket_conn_impl::websocket_conn_impl(http_server_impl& serv,
                                         std::unique_ptr<websocket_stream>&& stream,
                                         request&& req)

    : serv_(serv)
    , req_(std::move(req))
    , ws_(std::move(stream))
    , ac_que_(serv.get_executor())
{
}
websocket_conn_impl::~websocket_conn_impl()
{
}

void websocket_conn_impl::send_message(std::string&& msg, bool binary)
{
    if (!ws_->is_open())
        return;

    ac_que_.push(
        [this, msg = std::move(msg), binary, self = shared_from_this()]() -> net::awaitable<void> {
            if (binary)
                ws_->binary(true);
            else
                ws_->text(true);

            boost::system::error_code ec;
            co_await ws_->async_write(net::buffer(msg), net_awaitable[ec]);
        });
};
void websocket_conn_impl::send_ping(std::string&& msg)
{
    if (!ws_->is_open())
        return;

    ac_que_.push([this, msg = std::move(msg), self = shared_from_this()]() -> net::awaitable<void> {
        boost::system::error_code ec;
        co_await ws_->async_ping(beast::websocket::ping_data(std::string_view(msg)),
                                 net_awaitable[ec]);
    });
}

void websocket_conn_impl::close()
{
    if (!ws_->is_open())
        return;

    ac_que_.push([this, self = shared_from_this()]() -> net::awaitable<void> {
        boost::system::error_code ec;
        websocket::close_reason reason("normal");
        co_await ws_->async_close(reason, net_awaitable[ec]);
    });
}
httplib::net::awaitable<void> websocket_conn_impl::run()
{
    auto entry = serv_.router().query_ws_handler(req_);
    if (!entry)
        co_return;

    boost::system::error_code ec;
    auto remote_endp = ws_->socket().remote_endpoint(ec);

    co_await ws_->async_accept(req_, net_awaitable[ec]);
    if (ec) {
        serv_.get_logger()->error("websocket handshake failed: {}", ec.message());
        co_return;
    }

    co_await entry->open_handler(weak_from_this());

    serv_.get_logger()->debug(
        "websocket new connection: [{}:{}]", remote_endp.address().to_string(), remote_endp.port());


    for (;;) {
        auto bytes = co_await ws_->async_read(buffer_, net_awaitable[ec]);
        if (ec) {
            serv_.get_logger()->debug("websocket disconnect: [{}:{}] what: {}",
                                      remote_endp.address().to_string(),
                                      remote_endp.port(),
                                      ec.message());
            ac_que_.shutdown();

            co_await entry->close_handler(weak_from_this());
            co_return;
        }
        co_await entry->message_handler(
            weak_from_this(), util::buffer_to_string_view(buffer_.data()), ws_->got_binary());

        buffer_.consume(bytes);
    }
}


} // namespace httplib::server