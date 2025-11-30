#include "websocket_conn_impl.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <spdlog/spdlog.h>

namespace httplib::server {

websocket_conn_impl::websocket_conn_impl(http_server_impl& serv,
                                         websocket_variant_stream_type&& stream,
                                         request&& req)

    : serv_(serv)
    , req_(std::move(req))
    , strand_(stream.get_executor())
    , ws_(std::move(stream))
{
}


void websocket_conn_impl::send_message(std::string&& msg, data_type type)
{
    if (!ws_.is_open())
        return;

    net::post(strand_, [this, msg = std::move(msg), type, self = shared_from_this()]() mutable {
        bool send_in_process = !send_que_.empty();
        send_que_.push(std::make_pair(std::move(msg), type));
        if (send_in_process)
            return;
        net::co_spawn(strand_, process_write_data(), net::detached);
    });
}

void websocket_conn_impl::close()
{
    if (!ws_.is_open())
        return;

    net::co_spawn(
        strand_,
        [this, self = shared_from_this()]() -> net::awaitable<void> {
            boost::system::error_code ec;
            co_await net::post(strand_, net_awaitable[ec]);
            if (ec)
                co_return;

            websocket::close_reason reason("normal");
            co_await ws_.async_close(reason, net_awaitable[ec]);
        },
        net::detached);
}

httplib::net::awaitable<void> websocket_conn_impl::process_write_data()
{
    auto self = shared_from_this();
    boost::system::error_code ec;

    for (;;) {
        co_await net::post(strand_, net_awaitable[ec]);
        if (ec)
            co_return;
        if (send_que_.empty())
            co_return;

        const auto& msg = send_que_.front();

        if (msg.second == websocket_conn::data_type::text)
            ws_.text(true);
        else
            ws_.binary(true);
        co_await ws_.async_write(net::buffer(msg.first), net_awaitable[ec]);
        if (ec)
            co_return;

        co_await net::post(strand_, net_awaitable[ec]);
        if (ec)
            co_return;

        send_que_.pop();
    }
}

httplib::net::awaitable<void> websocket_conn_impl::run()
{
    auto entry = serv_.router().find_ws_handler(req_);
    if (!entry)
        co_return;

    boost::system::error_code ec;
    auto remote_endp = ws_.remote_endpoint(ec);

    co_await ws_.async_accept(req_, net_awaitable[ec]);
    if (ec) {
        serv_.get_logger()->error("websocket handshake failed: {}", ec.message());
        co_return;
    }

    co_await entry->open_handler(weak_from_this());

    serv_.get_logger()->debug(
        "websocket new connection: [{}:{}]", remote_endp.address().to_string(), remote_endp.port());

    beast::flat_buffer buffer;
    for (;;) {
        auto bytes = co_await ws_.async_read(buffer, net_awaitable[ec]);
        if (ec) {
            serv_.get_logger()->debug("websocket disconnect: [{}:{}] what: {}",
                                      remote_endp.address().to_string(),
                                      remote_endp.port(),
                                      ec.message());

            co_await entry->close_handler(weak_from_this());
            co_return;
        }


        co_await entry->message_handler(weak_from_this(),
                                        util::buffer_to_string_view(buffer.data()),
                                        ws_.got_text() ? websocket_conn::data_type::text
                                                       : websocket_conn::data_type::binary);

        buffer.consume(bytes);
    }
}

} // namespace httplib::server