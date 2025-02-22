
#pragma once
#include "httplib/config.hpp"
#include "httplib/util/type_traits.h"
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <variant>

namespace httplib
{

template<typename T>
constexpr inline bool is_awaitable_v = util::is_specialization_v<std::remove_cvref_t<T>, net::awaitable>;

template<typename... T>
class variant_handler : public std::variant<T...>
{
    using std::variant<T...>::variant;

public:
    template<typename... Args>
    net::awaitable<void> invoke(Args&&... args)
    {
        co_await std::visit(
            [&](auto& handler) mutable -> net::awaitable<void>
            {
                using handler_type = std::decay_t<decltype(handler)>;
                using return_type = typename util::function_traits<handler_type>::return_type;

                if constexpr (is_awaitable_v<return_type>)
                {
                    if (handler) co_await handler(std::forward<Args>(args)...);
                }
                else
                {
                    if (handler) handler(std::forward<Args>(args)...);
                }
                co_return;
            },
            *this);
        co_return;
    }
    template<typename... Args>
    net::awaitable<void> operator()(Args&&... args)
    {
        co_return co_await invoke(std::forward<Args>(args)...);
    }
    operator bool() const
    {
        return std::visit([&](auto& handler) { return !!handler; }, *this);
    }
};
} // namespace httplib