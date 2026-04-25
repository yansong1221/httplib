#include "httplib/client/ws_client.hpp"
#include "ws_client_impl.h"

namespace httplib::client {

ws_client::ws_client(net::io_context& ex,
                     std::string_view host,
                     uint16_t port,
                     bool ssl /*= false*/)
    : ws_client(ex.get_executor(), host, port, ssl)
{
}

ws_client::ws_client(const net::any_io_executor& ex,
                     std::string_view host,
                     uint16_t port,
                     bool ssl /*= false*/)
    : impl_(std::make_unique<ws_client::impl>(ex, host, port, ssl))
{
}
ws_client::~ws_client()
{
}
net::awaitable<boost::system::error_code> ws_client::async_connect(std::string_view path,
                                                                   const http::fields& headers)
{
    co_return co_await impl_->async_connect(path, headers);
}

bool ws_client::got_binary() const noexcept
{
    return impl_->got_binary();
}

bool ws_client::got_text() const noexcept
{
    return impl_->got_text();
}

httplib::net::awaitable<boost::system::error_code> ws_client::async_read()
{
    co_return co_await impl_->async_read();
}

httplib::net::awaitable<boost::system::error_code> ws_client::async_ping(std::string&& msg)
{
    co_return co_await impl_->async_ping(std::move(msg));
}

httplib::net::awaitable<boost::system::error_code> ws_client::async_pong(std::string&& msg)
{
    co_return co_await impl_->async_pong(std::move(msg));
}

httplib::net::awaitable<boost::system::error_code> ws_client::async_close()
{
    co_return co_await impl_->async_close();
}

httplib::net::awaitable<boost::system::error_code> ws_client::async_send(std::string&& data,
                                                                         bool binary /*= false*/)
{
    co_return co_await impl_->async_send(std::move(data), binary);
}

std::string_view ws_client::got_data() const noexcept
{
    return impl_->got_data();
}

void ws_client::set_handler_impl(coro_open_handler_type&& open_handler,
                                 coro_message_handler_type&& message_handler,
                                 coro_close_handler_type&& close_handler)
{
    return impl_->set_handler_impl(
        std::move(open_handler), std::move(message_handler), std::move(close_handler));
}

void ws_client::async_run(std::string_view path, const http::fields& headers /*= {}*/)
{
    return impl_->async_run(path, headers);
}

void ws_client::send(std::string&& data, bool binary /*= false*/)
{
    return impl_->send(std::move(data), binary);
}

void ws_client::close()
{
    return impl_->close();
}

} // namespace httplib::client