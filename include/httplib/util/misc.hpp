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
    /**
     * Convert a hex value to a decimal value.
     *
     * @param c The hexadecimal input.
     * @return The decimal output.
     */
    HTTPLIB_API std::uint8_t hex_to_dec(std::uint8_t c);

    /**
     * Decodes an URL.
     *
     * @details This function replaces %<hex> with the corresponding characters.
     *          See https://en.wikipedia.org/wiki/Percent-encoding
     *
     * @note As the replaced characters are "shorter" than the original input we can perform
     * the replacement in-place as long as we're somewhat careful not to fuck up.
     *
     * @param str The string to decode.
     */
    // NOTE: boost.url's pct_encode/pct_decode API requires charset+token,
    // overengineered for simple standalone string percent encoding.
    // Keeping hand-rolled version for simplicity.
    HTTPLIB_API void url_decode(std::string& str);
    HTTPLIB_API std::string url_decode(std::string_view str);
    HTTPLIB_API std::string url_encode(std::string_view value);
    HTTPLIB_API std::vector<std::string_view> split(std::string_view str,
                                                    std::string_view delimiter,
                                                    bool compress = true);

    HTTPLIB_API std::string_view buffer_to_string_view(boost::asio::const_buffer const& buffer);

    HTTPLIB_API std::string make_host_value(std::string_view host, uint16_t port, bool ssl);
    HTTPLIB_API std::string make_url_value(std::string_view host,
                                           uint16_t port,
                                           bool ssl,
                                           std::string_view target = {},
                                           std::string_view scheme = {});

} // namespace httplib::util
