#include "httplib/client/ws_client.hpp"
#include "ws_client_impl.h"

namespace httplib::client
{

    ws_client::ws_client(net::io_context& ex, std::string_view host, uint16_t port, scheme s /*= scheme::plain*/)
        : ws_client(ex.get_executor(), host, port, s)
    {
    }

    ws_client::ws_client(net::any_io_executor const& ex, std::string_view host, uint16_t port, scheme s /*= scheme::plain*/)
        : impl_(std::make_shared<ws_client::impl>(ex, host, port, s))
    {
    }
    ws_client::~ws_client() { abort(); }
    net::awaitable<boost::system::error_code>
    ws_client::async_connect(std::string_view target, http::fields const& headers)
    {
        boost::system::error_code ec;
        co_await impl_->async_connect(target, headers, ec);
        co_return ec;
    }

    bool
    ws_client::got_binary() const noexcept
    {
        return impl_->got_binary();
    }

    bool
    ws_client::got_text() const noexcept
    {
        return impl_->got_text();
    }

    httplib::net::awaitable<boost::system::error_code>
    ws_client::async_read()
    {
        boost::system::error_code ec;
        co_await impl_->async_read(ec);
        co_return ec;
    }

    httplib::net::awaitable<boost::system::error_code>
    ws_client::async_ping(std::string&& msg)
    {
        boost::system::error_code ec;
        co_await impl_->async_ping(msg, ec);
        co_return ec;
    }

    httplib::net::awaitable<boost::system::error_code>
    ws_client::async_pong(std::string&& msg)
    {
        boost::system::error_code ec;
        co_await impl_->async_pong(msg, ec);
        co_return ec;
    }

    httplib::net::awaitable<boost::system::error_code>
    ws_client::async_close()
    {
        boost::system::error_code ec;
        co_await impl_->async_close(ec);
        co_return ec;
    }

    httplib::net::awaitable<boost::system::error_code>
    ws_client::async_send(std::string&& data, bool binary /*= false*/)
    {
        boost::system::error_code ec;
        co_await impl_->async_send(data, binary, ec);
        co_return ec;
    }

    std::string_view
    ws_client::got_data() const noexcept
    {
        return impl_->got_data();
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

    void
    ws_client::send(std::string&& data, bool binary /*= false*/)
    {
        impl_->send(std::move(data), binary);
    }

    void
    ws_client::ping(std::string&& msg /*= std::string()*/)
    {
        impl_->ping(std::move(msg));
    }

    void
    ws_client::pong(std::string&& msg /*= std::string()*/)
    {
        impl_->pong(std::move(msg));
    }

    void
    ws_client::close()
    {
        impl_->close();
    }

    net::awaitable<boost::system::error_code>
    ws_client::async_run_impl(std::string_view target,
                              coro_message_handler_type&& message_handler,
                              coro_close_handler_type&& close_handler,
                              http::fields const& headers /*= {}*/)
    {
        boost::system::error_code ec;
        co_await impl_->async_run(target, headers, std::move(message_handler), std::move(close_handler), ec);
        co_return ec;
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

    bool
    ws_client::is_open() const noexcept
    {
        return impl_->is_open();
    }

    void
    ws_client::abort()
    {
        impl_->abort();
    }

} // namespace httplib::client