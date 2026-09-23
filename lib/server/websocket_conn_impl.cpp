#include "websocket_conn_impl.hpp"
#include "httplib/client/ws_client.hpp"
#include "request_impl.hpp"
#include "response_impl.hpp"
#include "ws_forward_impl.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_future.hpp>
#include <spdlog/spdlog.h>

namespace httplib::server
{

    websocket_conn_impl::websocket_conn_impl(std::shared_ptr<http_server::impl> server_impl,
                                             websocket_stream&& stream,
                                             request&& req)

        : server_impl_(std::move(server_impl))
        , req_(std::move(req))
        , ws_(std::move(stream))
        , write_mutex_(ws_.get_executor())
    {
    }
    websocket_conn_impl::~websocket_conn_impl() {}

    std::future<boost::system::error_code>
    websocket_conn_impl::send(websocket_message msg)
    {
        return net::co_spawn(
            ws_.get_executor(),
            [this, self = shared_from_this(), msg = std::move(msg)]() -> net::awaitable<boost::system::error_code>
            {
                boost::system::error_code ec;
                co_await async_send(msg, ec);
                co_return ec;
            },
            net::use_future);
    };

    net::awaitable<void>
    websocket_conn_impl::async_send(websocket_message const& msg, boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            ws_.get_executor(),
            [&]() -> net::awaitable<void>
            {
                if (!ws_.is_open())
                {
                    ec = net::error::make_error_code(net::error::not_connected);
                    co_return;
                }

                auto guard = co_await write_mutex_.lock();
                if (!guard)
                {
                    ec = net::error::make_error_code(net::error::operation_aborted);
                    co_return;
                }
                if (msg.is_binary())
                {
                    ws_.binary(true);
                }
                else
                {
                    ws_.text(true);
                }
                co_await ws_.async_write(net::buffer(msg.view()), util::net_awaitable[ec]);
                if (ec)
                {
                    co_await async_abort();
                }
            },
            net::use_awaitable);
    }

    std::future<boost::system::error_code>
    websocket_conn_impl::ping(std::string&& msg)
    {
        return net::co_spawn(
            ws_.get_executor(),
            [this, self = shared_from_this(), msg = std::move(msg)]() -> net::awaitable<boost::system::error_code>
            {
                boost::system::error_code ec;
                co_await async_ping(msg, ec);
                co_return ec;
            },
            net::use_future);
    }
    net::awaitable<void>
    websocket_conn_impl::async_ping(std::string_view msg, boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            ws_.get_executor(),
            [&]() -> net::awaitable<void>
            {
                if (!ws_.is_open())
                {
                    ec = net::error::make_error_code(net::error::not_connected);
                    co_return;
                }
                auto guard = co_await write_mutex_.lock();
                if (!guard)
                {
                    ec = net::error::make_error_code(net::error::operation_aborted);
                    co_return;
                }

                co_await ws_.async_ping(beast::websocket::ping_data(msg), util::net_awaitable[ec]);
                if (ec)
                {
                    co_await async_abort();
                }
            },
            net::use_awaitable);
    }
    std::future<boost::system::error_code>
    websocket_conn_impl::close(std::string_view reason)
    {
        return net::co_spawn(
            ws_.get_executor(),
            [this,
             self = shared_from_this(),
             reason = std::string(reason)]() -> net::awaitable<boost::system::error_code>
            {
                boost::system::error_code ec;
                co_await async_close(reason, ec);
                co_return ec;
            },
            net::use_future);
    }
    net::awaitable<void>
    websocket_conn_impl::async_close(std::string_view reason, boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            ws_.get_executor(),
            [&]() -> net::awaitable<void>
            {
                if (!ws_.is_open())
                {
                    ec = net::error::make_error_code(net::error::not_connected);
                    co_return;
                }
                auto guard = co_await write_mutex_.lock();
                if (!guard)
                {
                    ec = net::error::make_error_code(net::error::operation_aborted);
                    co_return;
                }

                using namespace net::experimental::awaitable_operators;
                using namespace std::chrono_literals;

                net::steady_timer timer(co_await net::this_coro::executor);
                timer.expires_after(5s);

                boost::system::error_code timer_ec;
                websocket::close_reason cr(std::move(reason));
                co_await (ws_.async_close(cr, util::net_awaitable[ec])
                          || timer.async_wait(util::net_awaitable[timer_ec]));

                if (ec && ec != net::error::operation_aborted)
                {
                    get_logger()->debug("websocket async_close failed: {}", ec.message());
                }

                co_await async_abort();
            },
            net::use_awaitable);
    }

    std::future<void>
    websocket_conn_impl::abort()
    {
        return net::co_spawn(
            ws_.get_executor(),
            [this, self = shared_from_this()]() -> net::awaitable<void>
            {
                co_await async_abort();
                co_return;
            },
            net::use_future);
    }
    net::awaitable<void>
    websocket_conn_impl::async_abort()
    {
        co_return co_await net::co_spawn(
            ws_.get_executor(),
            [&]() -> net::awaitable<void>
            {
                if (!ws_.is_open())
                {
                    co_return;
                }
                if (req_.data().has<detail::ws_forward_state_ptr>())
                {
                    auto state = req_.data().fetch<detail::ws_forward_state_ptr>();
                    if (state && state->upstream)
                    {
                        co_await state->upstream->async_abort();
                    }
                    detail::release_upstream_accounting(state);
                }

                boost::system::error_code ec;
                ws_.socket().shutdown(net::socket_base::shutdown_both, ec);
                ws_.socket().close(ec);
            },
            net::use_awaitable);
    }

    httplib::net::awaitable<void>
    websocket_conn_impl::run()
    {
        auto entry = server_impl_->router().query_ws_handler(req_);
        if (!entry)
        {
            co_return;
        }

        boost::system::error_code ec;
        auto remote_endp = ws_.socket().remote_endpoint(ec);

        ws_.set_option(websocket::permessage_deflate {});

        http::request<http::empty_body> req(get_impl(req_).header());
        co_await ws_.async_accept(req, util::net_awaitable[ec]);
        if (ec)
        {
            get_logger()->error("websocket handshake failed: {}", ec.message());
            co_return;
        }

        get_logger()->debug("websocket new connection: [{}:{}]", remote_endp.address().to_string(), remote_endp.port());

        try
        {
            co_await entry->open_handler(weak_from_this());
        }
        catch (std::exception const& e)
        {
            get_logger()->error("websocket open handler failed: {}", e.what());
            co_return;
        }

        for (;;)
        {
            websocket_message msg;
            auto dyn = net::dynamic_buffer(msg.data());
            co_await ws_.async_read(dyn, util::net_awaitable[ec]);
            if (ec)
            {
                get_logger()->debug("websocket disconnect: [{}:{}] what: {}",
                                    remote_endp.address().to_string(),
                                    remote_endp.port(),
                                    ec.message());

                co_await async_abort();
                break;
            }
            msg.set_binary(ws_.got_binary());
            try
            {
                co_await entry->message_handler(weak_from_this(), std::move(msg));
            }
            catch (std::exception const& e)
            {
                get_logger()->error("websocket message handler failed: {}", e.what());
            }
        }

        try
        {
            co_await entry->close_handler(weak_from_this());
        }
        catch (std::exception const& e)
        {
            get_logger()->error("websocket close handler failed: {}", e.what());
        }
    }

    std::shared_ptr<spdlog::logger>
    websocket_conn_impl::get_logger() const
    {
        return server_impl_->get_logger();
    }

} // namespace httplib::server