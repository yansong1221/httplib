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

} // namespace httplib::client