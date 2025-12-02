#pragma once
#include "httplib/util/misc.hpp"

namespace httplib::server {

namespace detail {

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

template<typename T>
net::awaitable<void> do_before(T& aspect, request& req, response& resp, bool& ok)
{
    if constexpr (has_before_v<T>) {
        if (!ok) {
            co_return;
        }
        using return_type = std::decay_t<decltype(aspect.before(req, resp))>;
        if constexpr (util::is_awaitable_v<return_type>)
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
        if constexpr (util::is_awaitable_v<return_type>)
            ok = co_await aspect.after(req, resp);
        else
            ok = aspect.after(req, resp);
    }
    co_return;
}

} // namespace detail


template<typename Func, typename... Aspects>
router::coro_http_handler_type router::make_coro_http_handler(Func&& handler, Aspects&&... asps)
{
    auto coro_handler = util::make_coro_handler(std::move(handler));
    if constexpr (sizeof...(Aspects) > 0) {
        return [coro_handler = std::move(coro_handler), ... asps = std::forward<Aspects>(asps)](
                   request& req, response& resp) mutable -> net::awaitable<void> {
            bool ok = true;
            co_await (detail::do_before(asps, req, resp, ok), ...);
            if (ok) {
                co_await coro_handler(req, resp);
            }
            ok = true;
            co_await (detail::do_after(asps, req, resp, ok), ...);
        };
    }
    else {
        return coro_handler;
    }
}


template<typename Func, typename... Aspects>
void router::set_http_handler(http::verb method,
                              std::string_view key,
                              Func&& handler,
                              Aspects&&... asps)
{
    set_http_handler_impl(method, key, make_coro_http_handler(std::move(handler), asps...));
}

template<http::verb... method, typename Func, typename... Aspects>
    requires std::is_member_function_pointer_v<Func>
void router::set_http_handler(std::string_view key,
                              Func&& handler,
                              util::class_type_t<Func>& owner,
                              Aspects&&... asps)
{
    using return_type = typename util::function_traits<Func>::return_type;

    using handler_type = std::
        conditional_t<util::is_awaitable_v<return_type>, coro_http_handler_type, http_handler_type>;

    handler_type f = std::bind(handler, &owner, std::placeholders::_1, std::placeholders::_2);
    set_http_handler<method...>(key, std::move(f), std::forward<Aspects>(asps)...);
}

template<typename Func, typename... Aspects>
void router::set_default_handler(Func&& handler, Aspects&&... asps)
{
    set_default_handler_impl(
        make_coro_http_handler(std::move(handler), std::forward<Aspects>(asps)...));
}

template<typename Func, typename... Aspects>
void router::set_file_request_handler(Func&& handler, Aspects&&... asps)
{
    set_file_request_handler_impl(
        make_coro_http_handler(std::move(handler), std::forward<Aspects>(asps)...));
}
template<typename OpenFunc, typename MessageFunc, typename CloseFunc>
void router::set_ws_handler(std::string_view key,
                            OpenFunc&& open_handler,
                            MessageFunc&& message_handler,
                            CloseFunc&& close_handler)
{
    set_ws_handler_impl(key,
                        util::make_coro_handler(std::move(open_handler)),
                        util::make_coro_handler(std::move(message_handler)),
                        util::make_coro_handler(std::move(close_handler)));
}

} // namespace httplib::server