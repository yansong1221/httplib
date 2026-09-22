#include "ws_client_impl.h"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "util/logging.hpp"
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <spdlog/spdlog.h>

namespace httplib::client
{
    ws_client::impl::impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, scheme s)
        : strand_(net::make_strand(ex))
        , host_(host)
        , port_(port)
        , scheme_(s)
        , write_mutex_(strand_)
        , read_mutex_(strand_)
        , httplib::detail::logger("httplib.ws_client")
    {
    }

    net::awaitable<void>
    ws_client::impl::async_connect(std::string_view target,
                                   http::fields const& headers,
                                   std::chrono::steady_clock::duration timeout,
                                   boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            strand_,
            [&]() -> net::awaitable<void>
            {
                co_await async_ping("ping", ec);
                if (!ec)
                {
                    co_return;
                }
                get_logger()->trace("connecting ws://{}:{}{}", host_, port_, target);

                auto ca_cert = ca_cert_.load();
                auto stream_result
                    = http_stream::create_stream(strand_,
                                                 host_,
                                                 scheme_ == scheme::tls,
                                                 verify_ssl_.load(),
                                                 ca_cert ? std::string_view(*ca_cert) : std::string_view {});
                if (!stream_result)
                {
                    ec = stream_result.error();
                    get_logger()->error("ws connect failed {}:{}: {}", host_, port_, ec.message());
                    co_return;
                }
                http_stream stream(std::move(*stream_result));
                tcp::resolver resolver(strand_);
                stream.expires_after(timeout);
                auto endpoints = co_await resolver.async_resolve(host_, std::to_string(port_), util::net_awaitable[ec]);
                if (ec)
                {
                    get_logger()->error("ws connect failed {}:{}: {}", host_, port_, ec.message());
                    co_return;
                }

                co_await stream.async_connect(endpoints, ec);
                if (ec)
                {
                    get_logger()->error("ws connect failed {}:{}: {}", host_, port_, ec.message());
                    co_return;
                }
                auto s = std::make_shared<websocket_stream>(std::move(stream));
                s->set_option(websocket::stream_base::decorator(
                    [&](websocket::request_type& req)
                    {
                        req.set(http::field::origin, util::make_url_value(host_, port_, scheme_));
                        req.set(http::field::host, util::make_host_value(host_, port_, scheme_));
                        req.set(http::field::user_agent,
                                std::string(BOOST_BEAST_VERSION_STRING) + "websocket-client-coro");
                        for (auto const& field : headers)
                        {
                            req.set(field.name_string(), field.value());
                        }
                    }));

                s->set_option(websocket::permessage_deflate {});
                co_await s->async_handshake(host_, target, util::net_awaitable[ec]);
                if (ec)
                {
                    get_logger()->error("ws connect failed {}:{}: {}", host_, port_, ec.message());
                    co_return;
                }
                s->expires_never();
                stream_.store(s);
                get_logger()->debug("ws connected to {}:{}{}", host_, port_, target);
                co_return;
            },
            net::use_awaitable);
    }

    bool
    ws_client::impl::is_open() const noexcept
    {
        auto s = stream_.load();
        return s && s->is_open();
    }

    httplib::net::awaitable<void>
    ws_client::impl::async_send(websocket_message const& msg, boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            strand_,
            [&]() -> net::awaitable<void>
            {
                auto s = get_stream(ec);
                if (ec)
                {
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
                    s->binary(true);
                }
                else
                {
                    s->text(true);
                }
                co_await s->async_write(net::buffer(msg.view()), util::net_awaitable[ec]);
                if (ec)
                {
                    get_logger()->error("Failed to send message: {}", ec.message());
                    co_await async_abort();
                }
            },
            net::use_awaitable);
    }

    std::future<boost::system::error_code>
    ws_client::impl::send(websocket_message msg)
    {
        return net::co_spawn(
            strand_,
            [this, self = shared_from_this(), msg = std::move(msg)]() -> net::awaitable<boost::system::error_code>
            {
                boost::system::error_code ec;
                co_await async_send(msg, ec);
                co_return ec;
            },
            net::use_future);
    }

    std::future<boost::system::error_code>
    ws_client::impl::ping(std::string&& msg /*= std::string()*/)
    {
        return net::co_spawn(
            strand_,
            [this, self = shared_from_this(), data = std::move(msg)]() -> net::awaitable<boost::system::error_code>
            {
                boost::system::error_code ec;
                co_await async_ping(data, ec);
                co_return ec;
            },
            net::use_future);
    }

    std::future<boost::system::error_code>
    ws_client::impl::pong(std::string&& msg /*= std::string()*/)
    {
        return net::co_spawn(
            strand_,
            [this, self = shared_from_this(), data = std::move(msg)]() -> net::awaitable<boost::system::error_code>
            {
                boost::system::error_code ec;
                co_await async_pong(data, ec);
                co_return ec;
            },
            net::use_future);
    }

    std::future<boost::system::error_code>
    ws_client::impl::close()
    {
        return net::co_spawn(
            strand_,
            [this, self = shared_from_this()]() -> net::awaitable<boost::system::error_code>
            {
                boost::system::error_code ec;
                co_await async_close(ec);
                co_return ec;
            },
            net::use_future);
    }

    httplib::net::awaitable<void>
    ws_client::impl::async_read(websocket_message& msg, boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            strand_,
            [&]() -> net::awaitable<void>
            {
                auto s = get_stream(ec);
                if (ec)
                {
                    co_return;
                }
                auto guard = co_await read_mutex_.lock();
                if (!guard)
                {
                    ec = net::error::make_error_code(net::error::operation_aborted);
                    co_return;
                }

                msg.data().clear();
                auto dyn = net::dynamic_buffer(msg.data());
                co_await s->async_read(dyn, util::net_awaitable[ec]);

                if (ec)
                {
                    if (ec != beast::websocket::error::closed)
                    {
                        get_logger()->warn("ws read failed: {}", ec.message());
                    }
                    co_await async_abort();
                }
                else
                {
                    msg.set_binary(s->got_binary());
                }
            },
            net::use_awaitable);
    }

    httplib::net::awaitable<void>
    ws_client::impl::async_ping(std::string_view msg, boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            strand_,
            [&]() -> net::awaitable<void>
            {
                auto s = get_stream(ec);
                if (ec)
                {
                    co_return;
                }
                auto guard = co_await write_mutex_.lock();
                if (!guard)
                {
                    ec = net::error::make_error_code(net::error::operation_aborted);
                    co_return;
                }
                co_await s->async_ping(beast::websocket::ping_data(msg), util::net_awaitable[ec]);
                if (ec)
                {
                    get_logger()->trace("Failed to send ping: {}", ec.message());
                    co_await async_abort();
                }
            },
            net::use_awaitable);
    }

    httplib::net::awaitable<void>
    ws_client::impl::async_pong(std::string_view msg, boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            strand_,
            [&]() -> net::awaitable<void>
            {
                auto s = get_stream(ec);
                if (ec)
                {
                    co_return;
                }
                auto guard = co_await write_mutex_.lock();
                if (!guard)
                {
                    ec = net::error::make_error_code(net::error::operation_aborted);
                    co_return;
                }
                co_await s->async_pong(beast::websocket::ping_data(msg), util::net_awaitable[ec]);
                if (ec)
                {
                    get_logger()->error("Failed to send pong: {}", ec.message());
                    co_await async_abort();
                }
            },
            net::use_awaitable);
    }

    httplib::net::awaitable<void>
    ws_client::impl::async_close(boost::system::error_code& ec)
    {
        co_return co_await net::co_spawn(
            strand_,
            [&]() -> net::awaitable<void>
            {
                auto s = get_stream(ec);
                if (ec)
                {
                    co_return;
                }
                auto guard = co_await write_mutex_.lock();
                if (!guard)
                {
                    ec = net::error::make_error_code(net::error::operation_aborted);
                    co_return;
                }

                using namespace boost::asio::experimental::awaitable_operators;
                using namespace std::chrono_literals;

                boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
                timer.expires_after(5s);

                boost::system::error_code timer_ec;
                websocket::close_reason reason("normal");
                co_await (s->async_close(reason, util::net_awaitable[ec])
                          || timer.async_wait(util::net_awaitable[timer_ec]));

                if (ec && ec != net::error::operation_aborted)
                {
                    get_logger()->error("Failed to close: {}", ec.message());
                }
                co_await async_abort();
            },
            net::use_awaitable);
    }

    void
    ws_client::impl::run(std::string_view target,
                         coro_open_handler_type&& open_handler,
                         coro_message_handler_type&& message_handler,
                         coro_close_handler_type&& close_handler,
                         http::fields const& headers /*= {}*/)
    {
        boost::asio::co_spawn(
            strand_,
            [this,
             self = shared_from_this(),
             target = std::string(target),
             headers,
             open_handler = std::move(open_handler),
             message_handler = std::move(message_handler),
             close_handler = std::move(close_handler)]() mutable -> net::awaitable<void>
            {
                boost::system::error_code ec;
                co_await async_connect(target, headers, std::chrono::seconds(30), ec);
                try
                {
                    co_await open_handler(ec);
                }
                catch (std::exception const& e)
                {
                    get_logger()->trace("ws open handler error: {}", e.what());
                }
                if (ec)
                {
                    co_return;
                }

                for (;;)
                {
                    websocket_message msg;
                    co_await async_read(msg, ec);
                    if (ec)
                    {
                        break;
                    }
                    try
                    {
                        co_await message_handler(std::move(msg));
                    }
                    catch (std::exception const& e)
                    {
                        get_logger()->error("ws message handler error: {}", e.what());
                    }
                }
                try
                {
                    co_await close_handler();
                }
                catch (std::exception const& e)
                {
                    get_logger()->error("ws close handler error: {}", e.what());
                }
                get_logger()->debug("ws connection closed");
            },
            [](std::exception_ptr e)
            {
                if (e)
                {
                    std::rethrow_exception(e);
                }
            });
    }

    net::awaitable<void>
    ws_client::impl::async_run(std::string_view target,
                               http::fields const& headers,
                               coro_message_handler_type&& message_handler,
                               coro_close_handler_type&& close_handler,
                               boost::system::error_code& ec)
    {
        co_await async_connect(target, headers, std::chrono::seconds(30), ec);
        if (ec)
        {
            co_return;
        }

        boost::asio::co_spawn(
            strand_,
            [this,
             self = shared_from_this(),
             message_handler = std::move(message_handler),
             close_handler = std::move(close_handler)]() -> net::awaitable<void>
            {
                boost::system::error_code ec;
                for (;;)
                {
                    websocket_message msg;
                    co_await async_read(msg, ec);
                    if (ec)
                    {
                        break;
                    }
                    try
                    {
                        co_await message_handler(std::move(msg));
                    }
                    catch (std::exception const& e)
                    {
                        get_logger()->error("ws message handler error: {}", e.what());
                    }
                }
                try
                {
                    co_await close_handler();
                }
                catch (std::exception const& e)
                {
                    get_logger()->error("ws close handler error: {}", e.what());
                }
                get_logger()->debug("ws connection closed");
            },
            net::detached);
    }

    std::future<void>
    ws_client::impl::abort()
    {
        return net::co_spawn(
            strand_,
            [this, self = shared_from_this()]() -> net::awaitable<void>
            {
                co_await async_abort();
                co_return;
            },
            net::use_future);
    }

    net::awaitable<void>
    ws_client::impl::async_abort()
    {
        co_await net::dispatch(strand_, net::use_awaitable);
        auto s = stream_.exchange(nullptr);
        if (!s)
        {
            co_return;
        }
        // 关闭底层 socket 会令在途的 beast 读写以 operation_aborted 完成，无需先 cancel。
        boost::system::error_code ec;
        s->socket().shutdown(net::socket_base::shutdown_both, ec);
        s->socket().close(ec);
    }

    std::shared_ptr<httplib::websocket_stream>
    ws_client::impl::get_stream(boost::system::error_code& ec) const
    {
        if (auto s = stream_.load(); s && s->is_open())
        {
            ec = {};
            return s;
        }
        ec = boost::system::errc::make_error_code(boost::system::errc::not_connected);
        return nullptr;
    }

} // namespace httplib::client