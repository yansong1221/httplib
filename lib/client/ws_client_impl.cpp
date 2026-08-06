#include "ws_client_impl.h"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace httplib::client
{
    ws_client::impl::impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl)
        : executor_(ex)
        , resolver_(ex)
        , host_(host)
        , port_(port)
        , use_ssl_(ssl)
        , ac_que_(ex)
    {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        spdlog::sinks_init_list sink_list = { console_sink };
        default_logger_ = std::make_shared<spdlog::logger>("httplib.ws_client", sink_list);
        default_logger_->set_level(spdlog::level::info);
    }

    net::awaitable<boost::system::error_code>
    ws_client::impl::async_connect(std::string_view target, http::fields const& headers)
    {
        logger()->trace("connecting ws://{}:{}{}", host_, port_, target);
        boost::system::error_code ec;

        if (!is_open())
        {
            auto stream_result = http_stream::create_stream(executor_, host_, use_ssl_);
            if (!stream_result)
            {
                logger()->error("ws connect failed {}:{}: {}", host_, port_, stream_result.error().message());
                co_return stream_result.error();
            }
            http_stream stream(std::move(*stream_result));
            auto endpoints = co_await resolver_.async_resolve(host_, std::to_string(port_), util::net_awaitable[ec]);
            if (ec)
            {
                logger()->error("ws connect failed {}:{}: {}", host_, port_, ec.message());
                co_return ec;
            }

            ec = co_await stream.async_connect(endpoints);
            if (ec)
            {
                logger()->error("ws connect failed {}:{}: {}", host_, port_, ec.message());
                co_return ec;
            }
            stream_ = std::make_shared<websocket_stream>(std::move(stream));
        }

        stream_->set_option(websocket::stream_base::decorator(
            [&](websocket::request_type& req)
            {
                req.set(http::field::origin, util::make_url_value(host_, port_, use_ssl_));
                req.set(http::field::host, util::make_host_value(host_, port_, use_ssl_));
                req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + "websocket-client-coro");
                for (auto const& field : headers)
                {
                    req.set(field.name_string(), field.value());
                }
            }));

        stream_->set_option(websocket::permessage_deflate {});

        co_await stream_->async_handshake(host_, target, util::net_awaitable[ec]);
        if (ec)
        {
            logger()->error("ws connect failed {}:{}: {}", host_, port_, ec.message());
            co_return ec;
        }

        logger()->debug("ws connected to {}:{}{}", host_, port_, target);
        co_return boost::system::error_code {};
    }

    bool
    ws_client::impl::is_open() const
    {
        return stream_ && stream_->is_open();
    }

    std::shared_ptr<spdlog::logger>
    ws_client::impl::logger() const
    {
        if (custom_logger_)
        {
            return custom_logger_;
        }
        return default_logger_;
    }

    void
    ws_client::impl::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        custom_logger_ = std::move(logger);
    }

    bool
    ws_client::impl::got_binary() const noexcept
    {
        return stream_ && stream_->got_binary();
    }

    bool
    ws_client::impl::got_text() const noexcept
    {
        return stream_ && stream_->got_text();
    }

    httplib::net::awaitable<boost::system::error_code>
    ws_client::impl::async_send(std::string&& data, bool binary /*= false*/)
    {
        if (!is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }

        if (binary)
        {
            stream_->binary(true);
        }
        else
        {
            stream_->text(true);
        }

        boost::system::error_code ec;
        co_await stream_->async_write(net::buffer(data), util::net_awaitable[ec]);
        co_return ec;
    }

    void
    ws_client::impl::send(std::string&& data, bool binary /*= false*/)
    {
        ac_que_.push(
            [this, data = std::move(data), binary]() mutable -> net::awaitable<void>
            {
                auto ec = co_await async_send(std::move(data), binary);
                if (ec)
                {
                    logger()->error("Failed to send message: {}", ec.message());
                }
            });
    }

    void
    ws_client::impl::ping(std::string&& msg /*= std::string()*/)
    {
        ac_que_.push(
            [this, data = std::move(msg)]() mutable -> net::awaitable<void>
            {
                auto ec = co_await async_ping(std::move(data));
                if (ec)
                {
                    logger()->error("Failed to send ping: {}", ec.message());
                }
            });
    }

    void
    ws_client::impl::pong(std::string&& msg /*= std::string()*/)
    {
        ac_que_.push(
            [this, data = std::move(msg)]() mutable -> net::awaitable<void>
            {
                auto ec = co_await async_pong(std::move(data));
                if (ec)
                {
                    logger()->error("Failed to send pong: {}", ec.message());
                }
            });
    }

    void
    ws_client::impl::close()
    {
        ac_que_.clear();
        ac_que_.push(
            [this]() mutable -> net::awaitable<void>
            {
                auto ec = co_await async_close();
                if (ec)
                {
                    logger()->error("Failed to close: {}", ec.message());
                }
            });
    }

    httplib::net::awaitable<boost::system::error_code>
    ws_client::impl::async_read()
    {
        if (!is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }
        buffer_.consume(buffer_.size());

        boost::system::error_code ec;
        co_await stream_->async_read(buffer_, util::net_awaitable[ec]);

        if (ec && ec != beast::websocket::error::closed)
        {
            logger()->warn("ws read failed: {}", ec.message());
        }
        co_return ec;
    }

    httplib::net::awaitable<boost::system::error_code>
    ws_client::impl::async_ping(std::string&& msg)
    {
        if (!is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }

        boost::system::error_code ec;
        co_await stream_->async_ping(beast::websocket::ping_data(std::string_view(msg)), util::net_awaitable[ec]);
        co_return ec;
    }

    httplib::net::awaitable<boost::system::error_code>
    ws_client::impl::async_pong(std::string&& msg)
    {
        if (!is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }

        boost::system::error_code ec;
        co_await stream_->async_pong(beast::websocket::ping_data(std::string_view(msg)), util::net_awaitable[ec]);
        co_return ec;
    }

    httplib::net::awaitable<boost::system::error_code>
    ws_client::impl::async_close()
    {
        if (!is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }

        using namespace boost::asio::experimental::awaitable_operators;
        using namespace std::chrono_literals;

        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
        timer.expires_after(5s);

        boost::system::error_code ec;
        websocket::close_reason reason("normal");
        co_await (stream_->async_close(reason, util::net_awaitable[ec]) || timer.async_wait(util::net_awaitable[ec]));
        if (!ec || ec == boost::asio::error::operation_aborted)
        {
            stream_->socket().shutdown(net::socket_base::shutdown_both, ec);
            stream_->socket().close(ec);
            co_return boost::system::error_code {};
        }

        co_return ec;
    }

    std::string_view
    ws_client::impl::got_data() const noexcept
    {
        return util::buffer_to_string_view(buffer_.data());
    }

    void
    ws_client::impl::set_handler_impl(coro_open_handler_type&& open_handler,
                                      coro_message_handler_type&& message_handler,
                                      coro_close_handler_type&& close_handler)
    {
        open_handler_ = std::move(open_handler);
        message_handler_ = std::move(message_handler);
        close_handler_ = std::move(close_handler);
    }

    void
    ws_client::impl::run(std::string_view path, http::fields const& headers /*= {}*/)
    {
        boost::asio::co_spawn(
            executor_,
            [this, self = shared_from_this(), path = std::string(path), headers]() -> net::awaitable<void>
            {
                try
                {
                    auto ec = co_await async_connect(path, headers);
                    co_await open_handler_(ec);
                    if (ec)
                    {
                        logger()->debug("ws open handler reported error, not reading");
                        co_return;
                    }

                    while (is_open())
                    {
                        auto read_ec = co_await async_read();
                        if (read_ec)
                        {
                            logger()->debug("ws read loop ended: {}", read_ec.message());
                            break;
                        }
                        if (message_handler_)
                        {
                            co_await message_handler_(got_data(), got_binary());
                        }
                    }

                    if (close_handler_)
                    {
                        co_await close_handler_();
                    }
                    logger()->debug("ws connection closed");
                }
                catch (std::exception const& e)
                {
                    logger()->warn("ws run exception: {}", e.what());
                }
                catch (...)
                {
                    logger()->warn("ws run unknown exception");
                }
            },
            boost::asio::detached);
    }

} // namespace httplib::client