#pragma once
#include "httplib/config.hpp"
#include "httplib/util/type_traits.h"
#include <boost/asio/buffer.hpp>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::util
{

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
            return [handler = std::forward<Func>(handler)](auto&&... args) -> net::awaitable<return_type>
            { co_return std::invoke(handler, args...); };
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

    HTTPLIB_API std::string_view buffer_to_string_view(boost::asio::const_buffer const& buffer);

} // namespace httplib::util
