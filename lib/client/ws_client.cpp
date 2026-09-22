#include "httplib/client/ws_client.hpp"
#include "ws_client_impl.h"

namespace httplib::client
{

    ws_client::ws_client(net::io_context& ex, std::string_view host, uint16_t port, scheme s /*= scheme::plain*/)
        : ws_client(ex.get_executor(), host, port, s)
    {
    }

    ws_client::ws_client(net::any_io_executor const& ex,
                         std::string_view host,
                         uint16_t port,
                         scheme s /*= scheme::plain*/)
        : impl_(std::make_shared<ws_client::impl>(ex, host, port, s))
    {
    }

    ws_client::~ws_client() { abort(); }

    net::awaitable<void>
    ws_client::async_send(websocket_message const& msg, boost::system::error_code& ec)
    {
        co_await impl_->async_send(msg, ec);
    }

    std::future<boost::system::error_code>
    ws_client::send(websocket_message msg)
    {
        return impl_->send(std::move(msg));
    }

    net::awaitable<void>
    ws_client::async_connect(std::string_view target,
                             http::fields const& headers,
                             std::chrono::steady_clock::duration timeout,
                             boost::system::error_code& ec)
    {
        co_await impl_->async_connect(target, headers, timeout, ec);
    }

    net::awaitable<void>
    ws_client::async_read(websocket_message& msg, boost::system::error_code& ec)
    {
        co_await impl_->async_read(msg, ec);
    }

    net::awaitable<void>
    ws_client::async_ping(std::string_view msg, boost::system::error_code& ec)
    {
        co_await impl_->async_ping(msg, ec);
    }

    std::future<boost::system::error_code>
    ws_client::ping(std::string&& msg /*= std::string()*/)
    {
        return impl_->ping(std::move(msg));
    }

    net::awaitable<void>
    ws_client::async_pong(std::string_view msg, boost::system::error_code& ec)
    {
        co_await impl_->async_pong(msg, ec);
    }

    std::future<boost::system::error_code>
    ws_client::pong(std::string&& msg /*= std::string()*/)
    {
        return impl_->pong(std::move(msg));
    }

    net::awaitable<void>
    ws_client::async_close(boost::system::error_code& ec)
    {
        co_await impl_->async_close(ec);
    }

    std::future<boost::system::error_code>
    ws_client::close()
    {
        return impl_->close();
    }

    net::awaitable<void>
    ws_client::async_abort()
    {
        co_await impl_->async_abort();
    }

    std::future<void>
    ws_client::abort()
    {
        return impl_->abort();
    }

    bool
    ws_client::is_open() const noexcept
    {
        return impl_->is_open();
    }

    void
    ws_client::set_verify_ssl(bool verify)
    {
        impl_->set_verify_ssl(verify);
    }

    void
    ws_client::set_ca_cert(std::string_view cert)
    {
        impl_->set_ca_cert(cert);
    }

    std::shared_ptr<spdlog::logger>
    ws_client::logger() const
    {
        return impl_->get_logger();
    }

    void
    ws_client::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        impl_->set_logger(std::move(logger));
    }

    net::awaitable<void>
    ws_client::async_run_impl(std::string_view target,
                              http::fields const& headers,
                              coro_message_handler_type&& message_handler,
                              coro_close_handler_type&& close_handler,
                              boost::system::error_code& ec)
    {
        co_await impl_->async_run(target, headers, std::move(message_handler), std::move(close_handler), ec);
    }

    void
    ws_client::run_impl(std::string_view target,
                        coro_open_handler_type&& open_handler,
                        coro_message_handler_type&& message_handler,
                        coro_close_handler_type&& close_handler,
                        http::fields const& headers /*= {}*/)
    {
        impl_->run(target, std::move(open_handler), std::move(message_handler), std::move(close_handler), headers);
    }

} // namespace httplib::client
