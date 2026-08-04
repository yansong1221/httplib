#include "websocket_conn_impl.hpp"
#include "request_impl.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <spdlog/spdlog.h>

namespace httplib::server
{

    websocket_conn_impl::websocket_conn_impl(http_server::impl& serv, websocket_stream&& stream, request&& req)

        : serv_(serv)
        , req_(std::move(req))
        , ws_(std::move(stream))
        , ac_que_(serv.get_executor())
    {
    }
    websocket_conn_impl::~websocket_conn_impl() {}

    void
    websocket_conn_impl::send(std::string&& msg, bool binary)
    {
        if (shutting_down_.load(std::memory_order_acquire) || !ws_.is_open())
        {
            return;
        }

        ac_que_.push(
            [this, msg = std::move(msg), binary, self = shared_from_this()]() -> net::awaitable<void>
            {
                if (binary)
                {
                    ws_.binary(true);
                }
                else
                {
                    ws_.text(true);
                }

                boost::system::error_code ec;
                co_await ws_.async_write(net::buffer(msg), util::net_awaitable[ec]);
            });
    };
    void
    websocket_conn_impl::ping(std::string&& msg)
    {
        if (shutting_down_.load(std::memory_order_acquire) || !ws_.is_open())
        {
            return;
        }

        ac_que_.push(
            [this, msg = std::move(msg), self = shared_from_this()]() -> net::awaitable<void>
            {
                boost::system::error_code ec;
                co_await ws_.async_ping(beast::websocket::ping_data(std::string_view(msg)), util::net_awaitable[ec]);
            });
    }

    void
    websocket_conn_impl::close(std::string_view reason)
    {
        if (shutting_down_.exchange(true, std::memory_order_acq_rel) || !ws_.is_open())
        {
            return;
        }

        ac_que_.push(
            [this, self = shared_from_this(), reason = std::string(reason)]() -> net::awaitable<void>
            {
                using namespace boost::asio::experimental::awaitable_operators;
                using namespace std::chrono_literals;

                boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
                timer.expires_after(5s);

                boost::system::error_code ec;
                websocket::close_reason cr(std::move(reason));
                co_await (ws_.async_close(cr, util::net_awaitable[ec]) || timer.async_wait(util::net_awaitable[ec]));

                if (ec && ec != boost::asio::error::operation_aborted)
                {
                    serv_.logger()->debug("websocket async_close failed: {}", ec.message());
                }

                ws_.socket().shutdown(net::socket_base::shutdown_both, ec);
                ws_.socket().close(ec);
            });
    }

    bool
    websocket_conn_impl::is_open() const
    {
        return !shutting_down_.load(std::memory_order_acquire) && ws_.is_open();
    }
    httplib::net::awaitable<void>
    websocket_conn_impl::run()
    {
        auto entry = serv_.router().query_ws_handler(req_);
        if (!entry)
        {
            co_return;
        }

        boost::system::error_code ec;
        auto remote_endp = ws_.socket().remote_endpoint(ec);

        co_await ws_.async_accept(get_impl(req_), util::net_awaitable[ec]);
        if (ec)
        {
            serv_.logger()->error("websocket handshake failed: {}", ec.message());
            co_return;
        }

        try
        {
            co_await entry->open_handler(weak_from_this());
        }
        catch (std::exception const& e)
        {
            serv_.logger()->error("websocket open handler failed: {}", e.what());
            co_return;
        }

        serv_.logger()->debug("websocket new connection: [{}:{}]",
                              remote_endp.address().to_string(),
                              remote_endp.port());

        for (;;)
        {
            auto bytes = co_await ws_.async_read(buffer_, util::net_awaitable[ec]);
            if (ec)
            {
                shutting_down_.store(true, std::memory_order_release);
                serv_.logger()->debug("websocket disconnect: [{}:{}] what: {}",
                                      remote_endp.address().to_string(),
                                      remote_endp.port(),
                                      ec.message());
                co_await ac_que_.async_shutdown();
                try
                {
                    co_await entry->close_handler(weak_from_this());
                }
                catch (std::exception const& e)
                {
                    serv_.logger()->error("websocket close handler failed: {}", e.what());
                }
                co_return;
            }
            try
            {
                co_await entry->message_handler(weak_from_this(),
                                                util::buffer_to_string_view(buffer_.data()),
                                                ws_.got_binary());
            }
            catch (std::exception const& e)
            {
                serv_.logger()->error("websocket message handler failed: {}", e.what());
            }
            buffer_.consume(bytes);
        }
    }

} // namespace httplib::server