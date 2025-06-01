#include "httplib/router.hpp"
#include "router_impl.h"

namespace httplib {

router::router()
    : impl_(new router_impl())
{
}
router::~router()
{
    delete impl_;
}
bool router::has_handler(http::verb method, std::string_view target) const
{
    return true;
}
net::awaitable<void> router::routing(request& req, response& resp)
{
    auto tokens = util::split(req.target(), "?");
    if (tokens.empty() || tokens.size() > 2) {
        resp.set_empty_content(http::status::bad_request);
        co_return;
    }
    req.path = util::url_decode(tokens[0]);
    if (tokens.size() >= 2) {
        bool is_valid    = true;
        req.query_params = html::parse_http_query_params(tokens[1], is_valid);
        if (!is_valid) {
            resp.set_empty_content(http::status::bad_request);
            co_return;
        }
    }
    co_await impl_->proc_routing(req, resp);
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

} // namespace httplib