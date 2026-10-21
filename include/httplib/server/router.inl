#pragma once
#include "httplib/server/server_fwd.hpp"
#include "httplib/util/misc.hpp"
#include <exception>
#include <functional>

namespace httplib::server
{

    namespace helper
    {

        template <class, class = void>
        struct has_before : std::false_type
        {
        };

        template <class T>
        struct has_before<
            T,
            std::void_t<decltype(std::declval<T>().before(std::declval<request&>(), std::declval<response&>()))>>
            : std::true_type
        {
        };

        template <class, class = void>
        struct has_after : std::false_type
        {
        };

        template <class T>
        struct has_after<
            T,
            std::void_t<decltype(std::declval<T>().after(std::declval<request&>(), std::declval<response&>()))>>
            : std::true_type
        {
        };

        template <class T>
        constexpr bool has_before_v = has_before<T>::value;

        /// aspect 通过 std::ref / std::cref 传入时是借用；其余按值拥有。
        template <typename T>
        struct is_reference_wrapper : std::false_type
        {
        };
        template <typename T>
        struct is_reference_wrapper<std::reference_wrapper<T>> : std::true_type
        {
        };

        /// 取出真正的 aspect 对象：reference_wrapper 解包，其余原样返回。
        template <typename T>
        decltype(auto)
        unwrap_aspect(T& aspect)
        {
            if constexpr (is_reference_wrapper<std::decay_t<T>>::value)
            {
                return aspect.get();
            }
            else
            {
                return (aspect);
            }
        }

        /// 注册期接管 aspect 的所有权：rvalue 移入，lvalue 拷贝入。
        ///
        /// 需要共享同一个有状态、不可拷贝的实例时，调用方应显式传 std::ref(instance)，
        /// 并保证 instance 的寿命长于 router——这条约束无法在编译期检查。
        template <typename T>
        std::shared_ptr<std::decay_t<T>>
        own_aspect(T&& aspect)
        {
            static_assert(std::is_constructible_v<std::decay_t<T>, T&&>,
                          "aspect is owned by value: pass std::ref(instance) to borrow a non-copyable "
                          "stateful aspect, and keep instance alive longer than the router");
            return std::make_shared<std::decay_t<T>>(std::forward<T>(aspect));
        }

        template <class T>
        constexpr bool has_after_v = has_after<T>::value;

        template <typename T>
        net::awaitable<void>
        do_before(T& aspect, request& req, response& resp, bool& ok)
        {
            if constexpr (has_before_v<T>)
            {
                if (!ok)
                {
                    co_return;
                }
                using return_type = std::decay_t<decltype(aspect.before(req, resp))>;
                if constexpr (util::is_awaitable_v<return_type>)
                {
                    ok = co_await aspect.before(req, resp);
                }
                else
                {
                    ok = aspect.before(req, resp);
                }
            }
            co_return;
        }

        template <typename T>
        net::awaitable<void>
        do_after(T& aspect, request& req, response& resp, bool& ok)
        {
            if constexpr (has_after_v<T>)
            {
                if (!ok)
                {
                    co_return;
                }
                using return_type = std::decay_t<decltype(aspect.after(req, resp))>;
                if constexpr (util::is_awaitable_v<return_type>)
                {
                    ok = co_await aspect.after(req, resp);
                }
                else
                {
                    ok = aspect.after(req, resp);
                }
            }
            co_return;
        }
    } // namespace helper

    template <typename... Aspects>
    void
    router::use(Aspects&&... asps)
    {
        auto do_use = [this]<typename T>(T&& asp)
        {
            auto m = helper::own_aspect(std::forward<T>(asp));
            use_impl(
                [m](request& req, response& resp) mutable -> net::awaitable<bool>
                {
                    bool ok = true;
                    co_await helper::do_before(helper::unwrap_aspect(*m), req, resp, ok);
                    co_return ok;
                },
                [m](request& req, response& resp) mutable -> net::awaitable<bool>
                {
                    bool ok = true;
                    co_await helper::do_after(helper::unwrap_aspect(*m), req, resp, ok);
                    co_return ok;
                });
        };
        (do_use(std::forward<Aspects>(asps)), ...);
    }

    template <typename Func, typename... Aspects>
    router::coro_http_handler_type
    router::make_coro_http_handler(Func&& handler, Aspects&&... asps)
    {
        auto coro_handler = util::make_coro_handler(std::forward<Func>(handler));

        if constexpr (sizeof...(Aspects) > 0)
        {
            // 注册即拥有：与 router::use() 一致，rvalue 移入、lvalue 拷贝入 shared_ptr。
            // 不能存 T&——传 lvalue 时闭包会长期持有调用方栈对象的地址。
            // 需要共享不可拷贝的有状态实例时用 std::ref(instance) 显式借用。
            auto aspects = std::make_tuple(helper::own_aspect(std::forward<Aspects>(asps))...);

            return [coro_handler = std::move(coro_handler),
                    aspects = std::move(aspects)](request& req, response& resp) mutable -> net::awaitable<void>
            {
                bool ok = true;

                co_await std::apply(
                    [&](auto&... aspect) -> net::awaitable<void>
                    { ((co_await helper::do_before(helper::unwrap_aspect(*aspect), req, resp, ok)), ...); },
                    aspects);

                std::exception_ptr eptr;
                if (ok)
                {
                    try
                    {
                        co_await coro_handler(req, resp);
                    }
                    catch (...)
                    {
                        eptr = std::current_exception();
                    }
                }

                // handler 正常返回（或 before 短路）才执行 after；抛异常时跳过，交给外层设 500
                if (!eptr)
                {
                    ok = true;
                    co_await std::apply(
                        [&](auto&... aspect) -> net::awaitable<void>
                        { ((co_await helper::do_after(helper::unwrap_aspect(*aspect), req, resp, ok)), ...); },
                        aspects);
                }

                if (eptr)
                {
                    std::rethrow_exception(eptr);
                }
            };
        }
        else
        {
            return coro_handler;
        }
    }

    template <typename Func, typename... Aspects>
    void
    router::set_http_handler(httplib::method method, std::string_view key, Func&& handler, Aspects&&... asps)
    {
        set_http_handler_impl(method,
                              key,
                              make_coro_http_handler(std::forward<Func>(handler), std::forward<Aspects>(asps)...));
    }

    template <httplib::method... method, typename Func, typename... Aspects>
        requires std::is_member_function_pointer_v<Func>
    void
    router::set_http_handler(std::string_view key, Func handler, util::class_type_t<Func>& owner, Aspects&&... asps)
    {
        using return_type = typename util::function_traits<Func>::return_type;

        using handler_type
            = std::conditional_t<util::is_awaitable_v<return_type>, coro_http_handler_type, http_handler_type>;

        handler_type f = std::bind(handler, &owner, std::placeholders::_1, std::placeholders::_2);
        set_http_handler<method...>(key, std::move(f), std::forward<Aspects>(asps)...);
    }

    template <httplib::method... method, typename Func, typename... Aspects>
        requires std::is_member_function_pointer_v<Func>
    void
    router::set_lazy_http_handler(std::string_view key,
                                  Func handler,
                                  util::class_type_t<Func>& owner,
                                  Aspects&&... asps)
    {
        using return_type = typename util::function_traits<Func>::return_type;

        using handler_type
            = std::conditional_t<util::is_awaitable_v<return_type>, coro_http_handler_type, http_handler_type>;

        handler_type f = std::bind(handler, &owner, std::placeholders::_1, std::placeholders::_2);
        set_lazy_http_handler<method...>(key, std::move(f), std::forward<Aspects>(asps)...);
    }

    template <typename Func, typename... Aspects>
    void
    router::set_http_not_found_handler(Func&& handler, Aspects&&... asps)
    {
        set_not_found_handler_impl(make_coro_http_handler(std::forward<Func>(handler), std::forward<Aspects>(asps)...));
    }

    template <typename OpenFunc, typename MessageFunc, typename CloseFunc, typename... Aspects>
    void
    router::set_ws_handler(std::string_view key,
                           OpenFunc&& open_handler,
                           MessageFunc&& message_handler,
                           CloseFunc&& close_handler)
    {
        set_ws_handler_impl(key,
                            util::make_coro_handler(std::forward<OpenFunc>(open_handler)),
                            util::make_coro_handler(std::forward<MessageFunc>(message_handler)),
                            util::make_coro_handler(std::forward<CloseFunc>(close_handler)));
    }

    template <typename... Aspects>
    void
    router::set_static_mount_point(std::string const& mount_point, fs::path const& dir, Aspects&&... asps)
    {
        set_static_mount_point(mount_point_entry(mount_point, dir), std::forward<Aspects>(asps)...);
    }
    template <typename... Aspects>
    void
    router::set_static_mount_point(mount_point_entry&& entry, Aspects&&... asps)
    {
        // 挂载点本身（不带尾斜杠）也要注册：只注册 "/mount/*" 时，GET /mount
        // 不会命中静态处理器，若用户另有全局兜底路由（例如 OPTIONS "/*"），
        // 路由会把它判成 405 而不是进入静态目录。裸路径由 mount_point_entry
        // 统一 301 到带尾斜杠的形式。
        std::string base = entry.mount_point();
        while (base.size() > 1 && base.back() == '/')
        {
            base.pop_back();
        }
        bool const is_root = base.empty() || base == "/";
        std::string const wildcard_key = is_root ? "/*" : base + "/*";

        // 同一个 entry 同时服务裸挂载点与通配路径；aspects 只构造一次，
        // 否则 rvalue aspect 会在第一次注册时被移走，第二次拿到 moved-from 对象。
        auto handler = make_coro_http_handler(
            [entry = std::move(entry)](request& req, response& resp) -> void
            {
                entry(req, resp);
            },
            std::forward<Aspects>(asps)...);

        auto register_handler = [&](httplib::method m, std::string_view route)
        {
            auto copy = handler;
            set_http_handler_impl(m, route, std::move(copy));
        };

        register_handler(httplib::method::get, wildcard_key);
        register_handler(httplib::method::head, wildcard_key);
        if (!is_root)
        {
            register_handler(httplib::method::get, base);
            register_handler(httplib::method::head, base);
        }
    }
    template <typename Func>
    void
    router::set_post_routing_handler(Func&& handler)
    {
        auto coro_handler = util::make_coro_handler(std::forward<Func>(handler));
        set_post_routing_handler_impl(std::move(coro_handler));
    }

    template <typename Func, typename... Aspects>
    void
    router::set_connect_handler(std::string_view key, Func&& handler, Aspects&&... asps)
    {
        // CONNECT 只是路由表中的一个普通动词：与其它 HTTP 方法共用注册/匹配/中间件管线。
        set_http_handler(httplib::method::connect, key, std::forward<Func>(handler), std::forward<Aspects>(asps)...);
    }

    template <typename Func, typename... Aspects>
    void
    router::set_lazy_http_handler(httplib::method method, std::string_view key, Func&& handler, Aspects... asps)
    {
        set_lazy_http_handler_impl(method,
                                   key,
                                   make_coro_http_handler(std::forward<Func>(handler), std::forward<Aspects>(asps)...));
    }

} // namespace httplib::server