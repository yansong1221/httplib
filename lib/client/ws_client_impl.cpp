#include "ws_client_impl.h"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace httplib::client {
ws_client::impl::impl(const net::any_io_executor& ex,
                      std::string_view host,
                      uint16_t port,
                      bool ssl)
    : executor_(ex)
    , resolver_(ex)
    , host_(host)
    , port_(port)
    , use_ssl_(ssl)
    , ac_que_(ex)
{
    auto console_sink                 = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::sinks_init_list sink_list = {console_sink};
    default_logger_ = std::make_shared<spdlog::logger>("httplib.ws_client", sink_list);
    default_logger_->set_level(spdlog::level::info);
}

net::awaitable<boost::system::error_code>
ws_client::impl::async_connect(std::string_view path, const http::fields& headers)
{
    try {
        logger()->trace("connecting ws://{}:{}{}", host_, port_, path);
        // Set up an HTTP GET request message
        if (!is_open()) {
            http_stream stream(executor_, host_, use_ssl_);
            auto endpoints =
                co_await resolver_.async_resolve(host_, std::to_string(port_), net::use_awaitable);

            co_await stream.async_connect(endpoints);
            stream_ = std::make_shared<websocket_stream>(std::move(stream));
        }
        stream_->set_option(websocket::stream_base::decorator([&](websocket::request_type& req) {
            req.set(http::field::user_agent,
                    std::string(BOOST_BEAST_VERSION_STRING) + "websocket-client-coro");
            for (const auto& field : headers)
                req.set(field.name_string(), field.value());
        }));

        co_await stream_->async_handshake(host_, path, net::use_awaitable);
        logger()->debug("ws connected to {}:{}{}", host_, port_, path);

        co_return boost::system::error_code {};
    }
    catch (const boost::system::system_error& e) {
        logger()->error("ws connect failed {}:{}: {}", host_, port_, e.what());
        co_return e.code();
    }
}

bool ws_client::impl::is_open() const
{
    return stream_ && stream_->is_open();
}

std::shared_ptr<spdlog::logger> ws_client::impl::logger() const
{
    if (custom_logger_)
        return custom_logger_;
    return default_logger_;
}

void ws_client::impl::set_logger(std::shared_ptr<spdlog::logger> logger)
{
    custom_logger_ = std::move(logger);
}

bool ws_client::impl::got_binary() const noexcept
{
    return stream_ && stream_->got_binary();
}

bool ws_client::impl::got_text() const noexcept
{
    return stream_ && stream_->got_text();
}

httplib::net::awaitable<boost::system::error_code>
ws_client::impl::async_send(std::string&& data, bool binary /*= false*/)
{
    try {
        if (!is_open()) {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }

        if (binary) {
            stream_->binary(true);
        }
        else {
            stream_->text(true);
        }
        co_await stream_->async_write(net::buffer(data), net::use_awaitable);

        co_return boost::system::error_code {};
    }
    catch (const boost::system::system_error& e) {
        co_return e.code();
    }
}

void ws_client::impl::send(std::string&& data, bool binary /*= false*/)
{
    ac_que_.push([this, data = std::move(data), binary]() mutable -> net::awaitable<void> {
        auto ec = co_await async_send(std::move(data), binary);
        if (ec) {
            logger()->error("Failed to send message: {}", ec.message());
        }
    });
}

void ws_client::impl::ping(std::string&& msg /*= std::string()*/)
{
    ac_que_.push([this, data = std::move(msg)]() mutable -> net::awaitable<void> {
        auto ec = co_await async_ping(std::move(data));
        if (ec) {
            logger()->error("Failed to send ping: {}", ec.message());
        }
    });
}

void ws_client::impl::pong(std::string&& msg /*= std::string()*/)
{
    ac_que_.push([this, data = std::move(msg)]() mutable -> net::awaitable<void> {
        auto ec = co_await async_pong(std::move(data));
        if (ec) {
            logger()->error("Failed to send pong: {}", ec.message());
        }
    });
}

void ws_client::impl::close()
{
    ac_que_.clear();
    ac_que_.push([this]() mutable -> net::awaitable<void> {
        auto ec = co_await async_close();
        if (ec) {
            logger()->error("Failed to close: {}", ec.message());
        }
    });
}


httplib::net::awaitable<boost::system::error_code> ws_client::impl::async_read()
{
    try {
        if (!is_open()) {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }
        buffer_.consume(buffer_.size());
        co_await stream_->async_read(buffer_, net::use_awaitable);

        co_return boost::system::error_code {};
    }
    catch (const boost::system::system_error& e) {
        if (e.code() != boost::asio::error::eof)
            logger()->warn("ws read failed: {}", e.what());
        co_return e.code();
    }
}

httplib::net::awaitable<boost::system::error_code> ws_client::impl::async_ping(std::string&& msg)
{
    try {
        if (!is_open()) {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }
        co_await stream_->async_ping(beast::websocket::ping_data(std::string_view(msg)),
                                     net::use_awaitable);
        co_return boost::system::error_code {};
    }
    catch (const boost::system::system_error& e) {
        co_return e.code();
    }
}

httplib::net::awaitable<boost::system::error_code> ws_client::impl::async_pong(std::string&& msg)
{
    try {
        if (!is_open()) {
            co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }
        co_await stream_->async_pong(beast::websocket::ping_data(std::string_view(msg)),
                                     net::use_awaitable);
        co_return boost::system::error_code {};
    }
    catch (const boost::system::system_error& e) {
        co_return e.code();
    }
}

httplib::net::awaitable<boost::system::error_code> ws_client::impl::async_close()
{
    if (!is_open()) {
        co_return boost::system::errc::make_error_code(boost::system::errc::not_connected);
    }

    using namespace boost::asio::experimental::awaitable_operators;
    using namespace std::chrono_literals;

    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    timer.expires_after(5s);

    boost::system::error_code ec;
    websocket::close_reason reason("normal");
    co_await (stream_->async_close(reason, util::net_awaitable[ec]) ||
              timer.async_wait(util::net_awaitable[ec]));
    if (!ec || ec == boost::asio::error::operation_aborted)
    {
        stream_->socket().shutdown(net::socket_base::shutdown_both, ec);
        stream_->socket().close(ec);
        co_return boost::system::error_code {};
    }

    co_return ec;
}

std::string_view ws_client::impl::got_data() const noexcept
{
    return util::buffer_to_string_view(buffer_.data());
}

void ws_client::impl::set_handler_impl(coro_open_handler_type&& open_handler,
                                       coro_message_handler_type&& message_handler,
                                       coro_close_handler_type&& close_handler)
{
    open_handler_    = std::move(open_handler);
    message_handler_ = std::move(message_handler);
    close_handler_   = std::move(close_handler);
}

void ws_client::impl::run(std::string_view path, const http::fields& headers /*= {}*/)
{
    boost::asio::co_spawn(
        executor_,
        [this, self = shared_from_this(), path = std::string(path), headers]()
            -> net::awaitable<void> {
            try {
                auto ec = co_await async_connect(path, headers);
                co_await open_handler_(ec);
                if (ec) {
                    logger()->debug("ws open handler reported error, not reading");
                    co_return;
                }

                while (is_open()) {
                    auto read_ec = co_await async_read();
                    if (read_ec) {
                        logger()->debug("ws read loop ended: {}", read_ec.message());
                        break;
                    }
                    if (message_handler_) {
                        co_await message_handler_(got_data(), got_binary());
                    }
                }

                if (close_handler_) {
                    co_await close_handler_();
                }
                logger()->debug("ws connection closed");
            }
            catch (const std::exception& e) {
                logger()->warn("ws run exception: {}", e.what());
            }
            catch (...) {
                logger()->warn("ws run unknown exception");
            }
        },
        boost::asio::detached);
}


} // namespace httplib::client