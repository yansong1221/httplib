#pragma once
#include "http_handler.hpp"
#include "type_traits.h"
#include <algorithm>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace httplib {
template<class, class = void>
struct has_before : std::false_type {};

template<class T>
struct has_before<T, std::void_t<decltype(std::declval<T>().before(std::declval<request &>(),
                                                                   std::declval<response &>()))>>
    : std::true_type {};

template<class, class = void>
struct has_after : std::false_type {};

template<class T>
struct has_after<T, std::void_t<decltype(std::declval<T>().after(std::declval<request &>(),
                                                                 std::declval<response &>()))>>
    : std::true_type {};

template<class T>
constexpr bool has_before_v = has_before<T>::value;

template<class T>
constexpr bool has_after_v = has_after<T>::value;

template<typename T>
constexpr inline bool is_awaitable_v =
    util::is_specialization_v<std::remove_cvref_t<T>, net::awaitable>;

namespace impl {
class router;
}

class router {
public:
    struct mount_point_entry {
        std::string mount_point;
        std::filesystem::path base_dir;
        http::fields headers;
    };

    explicit router(std::shared_ptr<spdlog::logger> logger);
    virtual ~router();

public:
    // eg: "GET hello/" as a key
    template<http::verb method, typename Func, typename... Aspects>
    void set_http_handler(std::string_view key, Func &&handler, Aspects &&...asps) {

        http_handler_variant handler_variant;
        using return_type =
            typename util::function_traits<std::decay_t<decltype(handler)>>::return_type;
        if constexpr (is_awaitable_v<return_type>) {
            handler_variant = coro_http_handler_type(std::move(handler));
        } else {
            handler_variant = http_handler_type(std::move(handler));
        }
        // hold keys to make sure map_handles_ key is
        // std::string_view, avoid memcpy when route
        coro_http_handler_type http_handler;
        if constexpr (sizeof...(Aspects) > 0) {
            http_handler = [this, handler = std::move(handler_variant),
                            ... asps = std::forward<Aspects>(asps)](
                               request &req, response &resp) mutable -> net::awaitable<void> {
                bool ok = true;
                (co_await do_before(asps, req, resp, ok), ...);
                if (ok) {
                    co_await handler(req, resp);
                }
                ok = true;
                (co_await do_after(asps, req, resp, ok), ...);
            };
        } else {
            http_handler = [this, handler = std::move(handler_variant)](
                               request &req, response &resp) mutable -> net::awaitable<void> {
                co_await handler(req, resp);
            };
        }
        set_http_handler(method, key, std::move(http_handler));
    }
    bool set_mount_point(const std::string &mount_point, const std::filesystem::path &dir,
                         const http::fields &headers = {});

    bool remove_mount_point(const std::string &mount_point);

    net::awaitable<void> routing(request &req, response &resp);

private:
    template<typename T>
    net::awaitable<void> do_before(T &aspect, request &req, response &resp, bool &ok) {
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
    net::awaitable<void> do_after(T &aspect, request &req, response &resp, bool &ok) {
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
    void set_http_handler(http::verb method, std::string_view key,
                          coro_http_handler_type &&handler);

private:
    impl::router *impl_;
};
} // namespace httplib