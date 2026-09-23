#pragma once
#include <cstdint>
#include <string_view>

// 参照 boost::url/scheme.hpp 依赖 boost/url/detail/config.hpp，本头同样依赖 config.hpp。
#include "httplib/config.hpp"

namespace httplib::url
{

    // 传输方案：显式枚举而非 bool，避免隐式转换（如 int/指针 -> bool）误传。
    enum class scheme : unsigned short
    {
        plain, // 明文（默认端口 80）
        tls    // 加密（默认端口 443）
    };

    /** 返回 scheme 对应的 URL 协议名（仿 boost::url::to_string）
     */
    constexpr std::string_view
    to_string(scheme s) noexcept
    {
        switch (s)
        {
            case scheme::plain: return "http";
            case scheme::tls: return "https";
        }
        return "http";
    }

    /** 返回 scheme 的默认端口（仿 boost::url::default_port）：plain=80，tls=443
     */
    constexpr uint16_t
    default_port(scheme s) noexcept
    {
        switch (s)
        {
            case scheme::plain: return 80;
            case scheme::tls: return 443;
        }
        return 80;
    }

} // namespace httplib::url