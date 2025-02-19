#pragma once
#include "httplib/config.hpp"
#include "httplib/util/type_traits.h"
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <variant>

namespace httplib {

template<typename T>
constexpr inline bool is_awaitable_v =
    util::is_specialization_v<std::remove_cvref_t<T>, net::awaitable>;

class request;
class response;

using coro_http_handler_type = std::function<net::awaitable<void>(request &req, response &resp)>;
using http_handler_type = std::function<void(request &req, response &resp)>;

class http_handler_variant : public std::variant<http_handler_type, coro_http_handler_type> {
    using std::variant<http_handler_type, coro_http_handler_type>::variant;

public:
    net::awaitable<void> invoke(request &req, response &resp) {
        co_await std::visit(
            [&](auto &handler) -> net::awaitable<void> {
                using handler_type = std::decay_t<decltype(handler)>;
                using return_type = typename util::function_traits<handler_type>::return_type;

                if constexpr (is_awaitable_v<return_type>) {
                    if (handler)
                        co_await handler(req, resp);
                } else {
                    if (handler)
                        handler(req, resp);
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