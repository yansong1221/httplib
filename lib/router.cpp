#include "httplib/router.hpp"
#include "router_impl.h"

namespace httplib {

router::router()
    : impl_(new router_impl())
{
}
router::~router()
{
   
}

net::awaitable<void> router::routing(request& req, response& resp)
{
    co_await impl_->proc_routing(req, resp);
}

std::optional<httplib::router::ws_handler_entry> router::find_ws_handler(std::string_view key) const
{
    return impl_->find_ws_handler(key); 
}

bool router::set_mount_point(const std::string& mount_point,
                             const fs::path& dir,
                             const http::fields& headers /*= {}*/)
{
    return impl_->set_mount_point(mount_point, dir, headers);
}
bool router::remove_mount_point(const std::string& mount_point)
{
    return impl_->remove_mount_point(mount_point);
}

bool router::has_handler(http::verb method, std::string_view target) const
{
    return true;
}

void router::set_http_handler_impl(http::verb method,
                                   std::string_view key,
                                   coro_http_handler_type&& handler)
{
    impl_->set_http_handler_impl(method, key, std::move(handler));
}

void router::set_default_handler_impl(coro_http_handler_type&& handler)
{
    impl_->set_default_handler_impl(std::move(handler));
}

void router::set_file_request_handler_impl(coro_http_handler_type&& handler)
{
    impl_->set_file_request_handler_impl(std::move(handler));
}

void router::set_ws_handler_impl(std::string_view key,
                                 websocket_conn::coro_open_handler_type&& open_handler,
                                 websocket_conn::coro_message_handler_type&& message_handler,
                                 websocket_conn::coro_close_handler_type&& close_handler)
{
    impl_->set_ws_handler_impl(
        key, std::move(open_handler), std::move(message_handler), std::move(close_handler));
}

} // namespace httplib