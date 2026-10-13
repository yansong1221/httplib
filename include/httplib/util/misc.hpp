#pragma once
#include "httplib/config.hpp"
#include "httplib/util/type_traits.h"
#include <charconv>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace httplib::util
{

    namespace detail
    {
        /** 立即完成的 awaitable，用来把同步 handler 的返回值包成可 await 的结果。 */
        inline net::awaitable<void>
        make_ready_awaitable()
        {
            co_return;
        }

        template <typename T>
        net::awaitable<T>
        make_ready_awaitable(T value)
        {
            co_return value;
        }
    } // namespace detail

    template <typename Func>
    static inline auto
    make_coro_handler(Func&& handler)
    {
        using return_type = typename util::function_traits<std::decay_t<decltype(handler)>>::return_type;
        if constexpr (is_awaitable_v<return_type>)
        {
            return std::forward<Func>(handler);
        }
        else
        {
            // 同步 handler 直接完美转发调用，再返回一个已完成的 awaitable。
            // 注意这里刻意不写成协程：net::awaitable 是懒启动的，若把调用放进
            // 协程体（原来的 `[handler](auto&&... args){ co_return ...; }`），
            // 协程帧里只会存下实参引用，等 co_await 启动时 std::function 的按值
            // 形参（例如 websocket_message）早已析构，消息就成了空的。
            return [handler = std::forward<Func>(handler)](auto&&... args) mutable -> net::awaitable<return_type>
            {
                if constexpr (std::is_void_v<return_type>)
                {
                    std::invoke(handler, std::forward<decltype(args)>(args)...);
                    return detail::make_ready_awaitable();
                }
                else
                {
                    return detail::make_ready_awaitable(std::invoke(handler, std::forward<decltype(args)>(args)...));
                }
            };
        }
    }
    template <typename T>
        requires std::integral<T> || std::floating_point<T>
    T
    from_chars_strict(std::string_view sv)
    {
        T val {};
        auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
        if (ec != std::errc {} || p != sv.data() + sv.size())
        {
            throw std::runtime_error("cannot convert '" + std::string(sv) + "'");
        }
        return val;
    }
    /** 按 delimiter 切分字符串；compress=true 时对每段做 trim，并跳过空段。
     */
    HTTPLIB_API std::vector<std::string_view> split(std::string_view str,
                                                    std::string_view delimiter,
                                                    bool compress = true);

} // namespace httplib::util
