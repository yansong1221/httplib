#pragma once
#include "helper.hpp"
#include "httplib/util/misc.hpp"

namespace httplib::server {

template<typename Func, typename... Aspects>
router::coro_http_handler_type router::make_coro_http_handler(Func&& handler, Aspects&&... asps)
{
    auto coro_handler = helper::make_coro_handler(std::forward<Func>(handler));


    if constexpr (sizeof...(Aspects) > 0) {
        // std::tuple<std::decay_t<Aspects>...> aspects(std::move(asps)...);
        std::tuple<Aspects...> aspects(std::forward<Aspects>(asps)...);

        return [coro_handler = std::move(coro_handler), aspects = std::move(aspects)](
                   request& req, response& resp) mutable -> net::awaitable<void> {
            bool ok = true;

            co_await std::apply(
                [&](auto&... aspect) -> net::awaitable<void> {
                    ((co_await helper::do_before(aspect, req, resp, ok)), ...);
                },
                aspects);

            if (ok) {
                co_await coro_handler(req, resp);
            }
            ok = true;

            co_await std::apply(
                [&](auto&... aspect) -> net::awaitable<void> {
                    ((co_await helper::do_after(aspect, req, resp, ok)), ...);
                },
                aspects);
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
    set_http_handler_impl(
        method,
        key,
        make_coro_http_handler(std::forward<Func>(handler), std::forward<Aspects>(asps)...));
}

template<http::verb... method, typename Func, typename... Aspects>
    requires std::is_member_function_pointer_v<Func>
void router::set_http_handler(std::string_view key,
                              Func&& handler,
                              util::class_type_t<Func>& owner,
                              Aspects&&... asps)
{
    using return_type = typename util::function_traits<Func>::return_type;

    using handler_type = std::conditional_t<helper::is_awaitable_v<return_type>,
                                            coro_http_handler_type,
                                            http_handler_type>;

    handler_type f = std::bind(handler, &owner, std::placeholders::_1, std::placeholders::_2);
    set_http_handler<method...>(key, std::move(f), std::forward<Aspects>(asps)...);
}

template<typename Func, typename... Aspects>
void router::set_http_not_found_handler(Func&& handler, Aspects&&... asps)
{
    set_not_found_handler_impl(
        make_coro_http_handler(std::forward<Func>(handler), std::forward<Aspects>(asps)...));
}

template<typename OpenFunc, typename MessageFunc, typename CloseFunc>
void router::set_ws_handler(std::string_view key,
                            OpenFunc&& open_handler,
                            MessageFunc&& message_handler,
                            CloseFunc&& close_handler)
{
    set_ws_handler_impl(key,
                        helper::make_coro_handler(std::forward<OpenFunc>(open_handler)),
                        helper::make_coro_handler(std::forward<MessageFunc>(message_handler)),
                        helper::make_coro_handler(std::forward<CloseFunc>(close_handler)));
}

template<typename... Aspects>
void router::set_static_mount_point(const std::string& mount_point,
                                    const fs::path& dir,
                                    Aspects&&... asps)
{
    set_static_mount_point(mount_point_entry(mount_point, dir), std::forward<Aspects>(asps)...);
}
template<typename... Aspects>
void router::set_static_mount_point(mount_point_entry&& entry, Aspects&&... asps)
{
    std::string key = entry.mount_point();
    if (!key.ends_with("/"))
        key += "/";
    key += "*";

    set_http_handler<http::verb::get, http::verb::head>(
        key,
        [entry = std::move(entry)](request& req, response& resp) -> void {
            entry(req, resp);
            return;
        },
        std::forward<Aspects>(asps)...);
}
template<typename Func>
void router::set_http_post_handler(Func&& handler)
{
    auto coro_handler = helper::make_coro_handler(std::forward<Func>(handler));
    set_http_post_handler_impl(std::move(coro_handler));
}

} // namespace httplib::server