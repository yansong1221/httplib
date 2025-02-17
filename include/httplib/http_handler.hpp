#pragma once
#include "message_variant.hpp"
namespace httplib {

using coro_http_handler_type = std::function<net::awaitable<void>(request &req, response &resp)>;
using http_handler_type = std::function<void(request &req, response &resp)>;

class http_handler_variant : public std::variant<http_handler_type, coro_http_handler_type> {
    using std::variant<http_handler_type, coro_http_handler_type>::variant;

public:
    net::awaitable<void> invoke(request &req, response &resp) {
        co_await std::visit(
            [&](auto &handler) -> net::awaitable<void> {
                using handler_type = std::decay_t<decltype(handler)>;
                if constexpr (std::same_as<handler_type, coro_http_handler_type>) {
                    if (handler)
                        co_await handler(req, resp);
                } else if constexpr (std::same_as<handler_type, http_handler_type>) {
                    if (handler)
                        handler(req, resp);
                } else {
                    static_assert(false, "unknown handler type");
                }
                co_return;
            },
            *this);
        co_return;
    }
    net::awaitable<void> operator()(request &req, response &resp) {
        co_return co_await invoke(req, resp);
    }
    operator bool() const {
        return std::visit([&](auto &handler) { return !!handler; }, *this);
    }
};

} // namespace httplib