#pragma once
#include "httplib/util/type_traits.h"
#include <boost/asio/awaitable.hpp>

namespace httplib::server {

class request;
class response;

namespace helper {

template<typename T>
constexpr inline bool is_awaitable_v =
    util::is_specialization_v<std::remove_cvref_t<T>, net::awaitable>;


template<class, class = void>
struct has_before : std::false_type
{
};

template<class T>
struct has_before<T,
                  std::void_t<decltype(std::declval<T>().before(
                      std::declval<request&>(), std::declval<response&>()))>> : std::true_type
{
};

template<class, class = void>
struct has_after : std::false_type
{
};

template<class T>
struct has_after<T,
                 std::void_t<decltype(std::declval<T>().after(
                     std::declval<request&>(), std::declval<response&>()))>> : std::true_type
{
};

template<class T>
constexpr bool has_before_v = has_before<T>::value;

template<class T>
constexpr bool has_after_v = has_after<T>::value;

template<typename Func>
static inline auto make_coro_handler(Func&& handler)
{
    using return_type =
        typename util::function_traits<std::decay_t<decltype(handler)>>::return_type;
    if constexpr (is_awaitable_v<return_type>) {
        return handler;
    }
    else {
        return [handler = std::move(handler)](auto&&... args) -> net::awaitable<return_type> {
            co_return std::invoke(handler, args...);
        };
    }
}

template<typename T>
net::awaitable<void> do_before(T& aspect, request& req, response& resp, bool& ok)
{
    if constexpr (has_before_v<T>) {
        if (!ok) {
            co_return;
        }
        using return_type = std::decay_t<decltype(aspect.before(req, resp))>;
        if constexpr (is_awaitable_v<return_type>)
            ok = co_await aspect.before(req, resp);
        else
            ok = aspect.before(req, resp);
    }
    co_return;
}

template<typename T>
net::awaitable<void> do_after(T& aspect, request& req, response& resp, bool& ok)
{
    if constexpr (has_after_v<T>) {
        if (!ok) {
            co_return;
        }
        using return_type = std::decay_t<decltype(aspect.after(req, resp))>;
        if constexpr (is_awaitable_v<return_type>)
            ok = co_await aspect.after(req, resp);
        else
            ok = aspect.after(req, resp);
    }
    co_return;
}
} // namespace helper


} // namespace httplib::server