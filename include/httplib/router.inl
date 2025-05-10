#pragma once
namespace httplib {

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
constexpr inline bool is_awaitable_v =
    util::is_specialization_v<std::remove_cvref_t<T>, net::awaitable>;

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
template<typename Func>
http_handler_variant create_http_handler_variant(Func&& handler)
{
    http_handler_variant handler_variant;
    using return_type =
        typename util::function_traits<std::decay_t<decltype(handler)>>::return_type;
    if constexpr (is_awaitable_v<return_type>) {
        handler_variant = coro_http_handler_type(std::move(handler));
    }
    else {
        handler_variant = http_handler_type(std::move(handler));
    }
    return std::move(handler_variant);
}
template<typename Func, typename... Aspects>
coro_http_handler_type create_router_coro_http_handler(Func&& handler, Aspects&&... asps)
{
    auto handler_variant = create_http_handler_variant(handler);
    // hold keys to make sure map_handles_ key is
    // std::string_view, avoid memcpy when route
    coro_http_handler_type http_handler;
    if constexpr (sizeof...(Aspects) > 0) {
        http_handler = [handler_variant = std::move(handler_variant),
                        ... asps        = std::forward<Aspects>(asps)](
                           request& req, response& resp) mutable -> net::awaitable<void> {
            bool ok = true;
            co_await (detail::do_before(asps, req, resp, ok), ...);
            if (ok) {
                co_await handler_variant(req, resp);
            }
            ok = true;
            co_await (detail::do_after(asps, req, resp, ok), ...);
        };
    }
    else {
        http_handler = [handler_variant = std::move(handler_variant)](
                           request& req, response& resp) mutable -> net::awaitable<void> {
            co_await handler_variant(req, resp);
        };
    }
    return std::move(http_handler);
}
} // namespace detail

template<typename Func, typename... Aspects>
void router::set_http_handler(http::verb method,
                              std::string_view key,
                              Func&& handler,
                              Aspects&&... asps)
{
    coro_http_handler_type http_handler =
        detail::create_router_coro_http_handler(std::move(handler), asps...);

    set_http_handler_impl(method, key, std::move(http_handler));
}

template<http::verb... method, typename Func, typename... Aspects>
void router::set_http_handler(std::string_view key,
                              Func&& handler,
                              util::class_type_t<Func>& owner,
                              Aspects&&... asps)
{
    static_assert(std::is_member_function_pointer_v<Func>, "must be member function");
    using return_type = typename util::function_traits<Func>::return_type;
    if constexpr (detail::is_awaitable_v<return_type>) {
        coro_http_handler_type f =
            std::bind(handler, &owner, std::placeholders::_1, std::placeholders::_2);
        set_http_handler<method...>(key, std::move(f), std::forward<Aspects>(asps)...);
    }
    else {
        http_handler_type f =
            std::bind(handler, &owner, std::placeholders::_1, std::placeholders::_2);
        set_http_handler<method...>(key, std::move(f), std::forward<Aspects>(asps)...);
    }
}

template<typename Func, typename... Aspects>
void router::set_default_handler(Func&& handler, Aspects&&... asps)
{
    auto coro_handler =
        detail::create_router_coro_http_handler(std::move(handler), std::forward<Aspects>(asps)...);
    this->set_default_handler_impl(std::move(coro_handler));
}

template<typename Func, typename... Aspects>
void router::set_file_request_handler(Func&& handler, Aspects&&... asps)
{
    auto coro_handler =
        detail::create_router_coro_http_handler(std::move(handler), std::forward<Aspects>(asps)...);
    set_file_request_handler_impl(std::move(coro_handler));
}

} // namespace httplib