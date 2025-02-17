#include "httplib/router.h"
#include "impl/router.hpp"

namespace httplib {

router::router(std::shared_ptr<spdlog::logger> logger) : impl_(new impl::router(logger)) {}

router::~router() {
    delete impl_;
}

net::awaitable<void> router::routing(request &req, response &resp) {
    co_await impl_->routing(req, resp);
    co_return;
}

bool router::set_mount_point(const std::string &mount_point, const std::filesystem::path &dir,
                             const http::fields &headers /*= {}*/) {
    return impl_->set_mount_point(mount_point, dir, headers);
}

bool router::remove_mount_point(const std::string &mount_point) {
    return impl_->remove_mount_point(mount_point);
}

void router::set_http_handler(http::verb method, std::string_view key,
                              coro_http_handler_type &&handler) {
    return impl_->set_http_handler(method, key, std::move(handler));
}

} // namespace httplib