#pragma once
#include "httplib/config.hpp"
#include "httplib/url/scheme.hpp"
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace httplib::url
{

    /// 解析后的绝对 URL 组成部分。集中接管 boost::urls::parse_uri，
    /// 供 client/server 各模块复用（缺省端口、TLS 推导、target() 提取）。
    struct HTTPLIB_API url_info
    {
        std::string scheme;   ///< 原始协议名，如 "http"/"https"/"ws"/"wss"
        std::string host;     ///< 主机名/IP；IPv6 字面量不含方括号
        uint16_t port = 0;    ///< 显式端口；未指定时为 0
        std::string path;     ///< 编码后的 path，缺省为空串
        std::string query;    ///< 编码后的 query（不含 '?'），缺省为空串
        std::string fragment; ///< 编码后的 fragment（不含 '#'），缺省为空串

        bool
        has_port() const noexcept
        {
            return port != 0;
        }

        /// https/wss -> tls，其余 -> plain
        bool is_ssl() const noexcept;

        url::scheme
        transport() const noexcept
        {
            return is_ssl() ? url::scheme::tls : url::scheme::plain;
        }

        /// 显式端口；缺省时按 scheme 返回默认端口（plain=80 / tls=443）。
        uint16_t
        effective_port() const noexcept
        {
            return port ? port : default_port(transport());
        }

        /// origin-form：path + "?" + query + ["#" + fragment]。
        /// path 缺省时补 "/"（authority-form 规范化）。
        /// include_fragment=true 时含 fragment（仿 boost::urls::url::target()）；
        /// 置 false 得到请求目标（fragment 不进请求行，RFC 9110）。
        std::string target(bool include_fragment = true) const;

        /// 拼回完整 URL：scheme://host[:port]path[?query][#fragment]。
        /// port==0（未显式指定）或等于该 scheme 默认端口（80/443）时省略端口；IPv6 字面量自动补方括号。
        std::string to_url() const;
    };

    /// 解析绝对 URL。失败时返回 boost::urls 的解析错误码。
    HTTPLIB_API std::expected<url_info, boost::system::error_code> parse_url(std::string_view url);

    /// 以 base_target（绝对 path[?query]）为基准，按 RFC 3986 §5.2 解析（可能相对的）location，
    /// 返回同 authority 的请求目标 path[?query]（不含 fragment）。location 为空时返回 base_target 的
    /// target；无法解析时回退返回 base_target。location 为绝对/scheme-relative 时其 authority 变化
    /// 不在此函数契约内（仅取其 path）。
    HTTPLIB_API std::string resolve(std::string_view base_target, std::string_view location);

    /// 拼装 Host 头 / 连接地址：host[:port]，非默认端口省略端口；IPv6 字面量自动加方括号。
    HTTPLIB_API std::string make_host_value(std::string_view host, uint16_t port, url::scheme s);

    /// 拼装完整 URL：scheme://host[:port]target。url_scheme 非空时覆盖默认协议名。
    HTTPLIB_API std::string make_url_value(std::string_view host,
                                           uint16_t port,
                                           url::scheme s,
                                           std::string_view target = {},
                                           std::string_view url_scheme = {});

    /**
     * Decodes an URL, replacing %<hex> with the corresponding characters.
     * See https://en.wikipedia.org/wiki/Percent-encoding
     *
     * @note As the replaced characters are "shorter" than the original input we can perform
     * the replacement in-place as long as we're somewhat careful not to fuck up.
     */
    // NOTE: boost.url's pct_encode/pct_decode API requires charset+token,
    // overengineered for simple standalone string percent encoding.
    // Keeping hand-rolled version for simplicity.
    HTTPLIB_API void url_decode(std::string& str);
    HTTPLIB_API std::string url_decode(std::string_view str);
    HTTPLIB_API std::string url_encode(std::string_view value);

} // namespace httplib::url