#pragma once

// 公共的 httplib::{method,status} 与 beast 的 http::{verb,status} 之间的转换入口，
// 以及 httplib::field 到线上字段名的字面值表。本头是 lib 私有头（include/httplib/
// 里不能出现 beast）。
//
// method/status 一律走全覆盖 constexpr switch，勿用 static_cast：两套枚举各自独立
// 声明，static_cast 在值失配时照样编译通过，而这两个枚举直接决定线上字节，错一位
// 就是协议破坏且只在运行时暴露。不写 default，末尾单独 return 兜底，枚举外的数值
// 退回 unknown 而不是 UB。
//
// 字段不走 Beast 的 http::field：它的集合在 1.89 / 1.90 / 1.92 之间发生过增删，
// 按枚举名映射会随 Boost 版本漂移。这里直接维护「本库 field -> 线上字段名」的
// 字面值表，头操作一律走 Beast 的 string_view 接口。
//
// ---- 名字表 ----
//
// method/status 每个名字只列一次，X-macro 展开出正向 / 反向两组 switch；字段表
// 同时供 field_name() 与 field_from_name() 使用。加枚举值 = 在对应表里加一行。
//
// 表区域关掉 clang-format：续行反斜杠的列对齐靠手工维护，格式化会把它搅乱。

#include "httplib/config.hpp"
#include "httplib/enums.hpp"

#include "beast_alias.hpp"

#include <boost/beast/core/string.hpp>
#include <string_view>

namespace httplib::enum_conv
{

    // clang-format off

#define HTTPLIB_VERB_TABLE(X) \
    X(unknown)                                                                                     \
    X(delete_)                                                                                     \
    X(get)                                                                                         \
    X(head)                                                                                        \
    X(post)                                                                                        \
    X(put)                                                                                         \
    X(connect)                                                                                     \
    X(options)                                                                                     \
    X(trace)                                                                                       \
    X(copy)                                                                                        \
    X(lock)                                                                                        \
    X(mkcol)                                                                                       \
    X(move)                                                                                        \
    X(propfind)                                                                                    \
    X(proppatch)                                                                                   \
    X(search)                                                                                      \
    X(unlock)                                                                                      \
    X(bind)                                                                                        \
    X(rebind)                                                                                      \
    X(unbind)                                                                                      \
    X(acl)                                                                                         \
    X(report)                                                                                      \
    X(mkactivity)                                                                                  \
    X(checkout)                                                                                    \
    X(merge)                                                                                       \
    X(msearch)                                                                                     \
    X(notify)                                                                                      \
    X(subscribe)                                                                                   \
    X(unsubscribe)                                                                                 \
    X(patch)                                                                                       \
    X(purge)                                                                                       \
    X(mkcalendar)                                                                                  \
    X(link)                                                                                        \
    X(unlink)

#define HTTPLIB_STATUS_TABLE(X) \
    X(unknown)                                                                                     \
    X(continue_)                                                                                   \
    X(switching_protocols)                                                                         \
    X(processing)                                                                                  \
    X(early_hints)                                                                                 \
    X(ok)                                                                                          \
    X(created)                                                                                     \
    X(accepted)                                                                                    \
    X(non_authoritative_information)                                                               \
    X(no_content)                                                                                  \
    X(reset_content)                                                                               \
    X(partial_content)                                                                             \
    X(multi_status)                                                                                \
    X(already_reported)                                                                            \
    X(im_used)                                                                                     \
    X(multiple_choices)                                                                            \
    X(moved_permanently)                                                                           \
    X(found)                                                                                       \
    X(see_other)                                                                                   \
    X(not_modified)                                                                                \
    X(use_proxy)                                                                                   \
    X(temporary_redirect)                                                                          \
    X(permanent_redirect)                                                                          \
    X(bad_request)                                                                                 \
    X(unauthorized)                                                                                \
    X(payment_required)                                                                            \
    X(forbidden)                                                                                   \
    X(not_found)                                                                                   \
    X(method_not_allowed)                                                                          \
    X(not_acceptable)                                                                              \
    X(proxy_authentication_required)                                                               \
    X(request_timeout)                                                                             \
    X(conflict)                                                                                    \
    X(gone)                                                                                        \
    X(length_required)                                                                             \
    X(precondition_failed)                                                                         \
    X(payload_too_large)                                                                           \
    X(uri_too_long)                                                                                \
    X(unsupported_media_type)                                                                      \
    X(range_not_satisfiable)                                                                       \
    X(expectation_failed)                                                                          \
    X(i_am_a_teapot)                                                                               \
    X(misdirected_request)                                                                         \
    X(unprocessable_entity)                                                                        \
    X(locked)                                                                                      \
    X(failed_dependency)                                                                           \
    X(too_early)                                                                                   \
    X(upgrade_required)                                                                            \
    X(precondition_required)                                                                       \
    X(too_many_requests)                                                                           \
    X(request_header_fields_too_large)                                                             \
    X(unavailable_for_legal_reasons)                                                               \
    X(internal_server_error)                                                                       \
    X(not_implemented)                                                                             \
    X(bad_gateway)                                                                                 \
    X(service_unavailable)                                                                         \
    X(gateway_timeout)                                                                             \
    X(http_version_not_supported)                                                                  \
    X(variant_also_negotiates)                                                                     \
    X(insufficient_storage)                                                                        \
    X(loop_detected)                                                                               \
    X(not_extended)                                                                                \
    X(network_authentication_required)

#define HTTPLIB_FIELD_TABLE(X) \
    X(accept, "Accept") \
    X(accept_ch, "Accept-CH") \
    X(accept_charset, "Accept-Charset") \
    X(accept_encoding, "Accept-Encoding") \
    X(accept_language, "Accept-Language") \
    X(accept_patch, "Accept-Patch") \
    X(accept_post, "Accept-Post") \
    X(accept_ranges, "Accept-Ranges") \
    X(accept_signature, "Accept-Signature") \
    X(access_control_allow_credentials, "Access-Control-Allow-Credentials") \
    X(access_control_allow_headers, "Access-Control-Allow-Headers") \
    X(access_control_allow_methods, "Access-Control-Allow-Methods") \
    X(access_control_allow_origin, "Access-Control-Allow-Origin") \
    X(access_control_expose_headers, "Access-Control-Expose-Headers") \
    X(access_control_max_age, "Access-Control-Max-Age") \
    X(access_control_request_headers, "Access-Control-Request-Headers") \
    X(access_control_request_method, "Access-Control-Request-Method") \
    X(age, "Age") \
    X(allow, "Allow") \
    X(alt_svc, "Alt-Svc") \
    X(alt_used, "Alt-Used") \
    X(authorization, "Authorization") \
    X(cache_control, "Cache-Control") \
    X(clear_site_data, "Clear-Site-Data") \
    X(connection, "Connection") \
    X(content_digest, "Content-Digest") \
    X(content_disposition, "Content-Disposition") \
    X(content_dpr, "Content-DPR") \
    X(content_encoding, "Content-Encoding") \
    X(content_language, "Content-Language") \
    X(content_length, "Content-Length") \
    X(content_location, "Content-Location") \
    X(content_range, "Content-Range") \
    X(content_security_policy, "Content-Security-Policy") \
    X(content_security_policy_report_only, "Content-Security-Policy-Report-Only") \
    X(content_type, "Content-Type") \
    X(cookie, "Cookie") \
    X(cross_origin_embedder_policy, "Cross-Origin-Embedder-Policy") \
    X(cross_origin_opener_policy, "Cross-Origin-Opener-Policy") \
    X(cross_origin_resource_policy, "Cross-Origin-Resource-Policy") \
    X(date, "Date") \
    X(deprecation, "Deprecation") \
    X(device_memory, "Device-Memory") \
    X(digest, "Digest") \
    X(dnt, "DNT") \
    X(dpr, "DPR") \
    X(etag, "ETag") \
    X(expect, "Expect") \
    X(expect_ct, "Expect-CT") \
    X(expires, "Expires") \
    X(forwarded, "Forwarded") \
    X(from, "From") \
    X(host, "Host") \
    X(http2_settings, "HTTP2-Settings") \
    X(if_match, "If-Match") \
    X(if_modified_since, "If-Modified-Since") \
    X(if_none_match, "If-None-Match") \
    X(if_range, "If-Range") \
    X(if_unmodified_since, "If-Unmodified-Since") \
    X(keep_alive, "Keep-Alive") \
    X(last_modified, "Last-Modified") \
    X(link, "Link") \
    X(location, "Location") \
    X(max_forwards, "Max-Forwards") \
    X(origin, "Origin") \
    X(origin_agent_cluster, "Origin-Agent-Cluster") \
    X(pragma, "Pragma") \
    X(prefer, "Prefer") \
    X(preference_applied, "Preference-Applied") \
    X(priority, "Priority") \
    X(proxy_authenticate, "Proxy-Authenticate") \
    X(proxy_authorization, "Proxy-Authorization") \
    X(proxy_connection, "Proxy-Connection") \
    X(range, "Range") \
    X(referer, "Referer") \
    X(referrer_policy, "Referrer-Policy") \
    X(refresh, "Refresh") \
    X(report_to, "Report-To") \
    X(reporting_endpoints, "Reporting-Endpoints") \
    X(repr_digest, "Repr-Digest") \
    X(retry_after, "Retry-After") \
    X(sec_ch_ua_full_version, "Sec-CH-UA-Full-Version") \
    X(sec_fetch_dest, "Sec-Fetch-Dest") \
    X(sec_fetch_mode, "Sec-Fetch-Mode") \
    X(sec_fetch_site, "Sec-Fetch-Site") \
    X(sec_fetch_user, "Sec-Fetch-User") \
    X(sec_purpose, "Sec-Purpose") \
    X(sec_websocket_accept, "Sec-WebSocket-Accept") \
    X(sec_websocket_extensions, "Sec-WebSocket-Extensions") \
    X(sec_websocket_key, "Sec-WebSocket-Key") \
    X(sec_websocket_protocol, "Sec-WebSocket-Protocol") \
    X(sec_websocket_version, "Sec-WebSocket-Version") \
    X(server, "Server") \
    X(server_timing, "Server-Timing") \
    X(service_worker, "Service-Worker") \
    X(service_worker_allowed, "Service-Worker-Allowed") \
    X(service_worker_navigation_preload, "Service-Worker-Navigation-Preload") \
    X(set_cookie, "Set-Cookie") \
    X(set_login, "Set-Login") \
    X(signature, "Signature") \
    X(signature_input, "Signature-Input") \
    X(sourcemap, "SourceMap") \
    X(strict_transport_security, "Strict-Transport-Security") \
    X(te, "TE") \
    X(timing_allow_origin, "Timing-Allow-Origin") \
    X(tk, "Tk") \
    X(trailer, "Trailer") \
    X(transfer_encoding, "Transfer-Encoding") \
    X(upgrade, "Upgrade") \
    X(upgrade_insecure_requests, "Upgrade-Insecure-Requests") \
    X(user_agent, "User-Agent") \
    X(vary, "Vary") \
    X(via, "Via") \
    X(viewport_width, "Viewport-Width") \
    X(want_content_digest, "Want-Content-Digest") \
    X(want_repr_digest, "Want-Repr-Digest") \
    X(warning, "Warning") \
    X(width, "Width") \
    X(www_authenticate, "WWW-Authenticate") \
    X(x_content_type_options, "X-Content-Type-Options") \
    X(x_dns_prefetch_control, "X-DNS-Prefetch-Control") \
    X(x_forwarded_for, "X-Forwarded-For") \
    X(x_forwarded_host, "X-Forwarded-Host") \
    X(x_forwarded_proto, "X-Forwarded-Proto") \
    X(x_frame_options, "X-Frame-Options") \
    X(x_permitted_cross_domain_policies, "X-Permitted-Cross-Domain-Policies") \
    X(x_powered_by, "X-Powered-By") \
    X(x_robots_tag, "X-Robots-Tag") \
    X(x_xss_protection, "X-XSS-Protection")

    // clang-format on

    // ---- 方法 ----

    /// method -> http::verb
#define HTTPLIB_FWD(N) case method::N: return http::verb::N;
    constexpr http::verb
    to_verb(method v) noexcept
    {
        switch (v)
        {
            HTTPLIB_VERB_TABLE(HTTPLIB_FWD)
        }
        return http::verb::unknown;
    }
#undef HTTPLIB_FWD

    /// http::verb -> method
#define HTTPLIB_REV(N) case http::verb::N: return method::N;
    constexpr method
    to_method(http::verb v) noexcept
    {
        switch (v)
        {
            HTTPLIB_VERB_TABLE(HTTPLIB_REV)
        }
        return method::unknown;
    }
#undef HTTPLIB_REV

    // ---- 状态码 ----

    /// status -> http::status
#define HTTPLIB_FWD(N) case status::N: return http::status::N;
    constexpr http::status
    to_status(status v) noexcept
    {
        switch (v)
        {
            HTTPLIB_STATUS_TABLE(HTTPLIB_FWD)
        }
        return http::status::unknown;
    }
#undef HTTPLIB_FWD

    /// http::status -> status
#define HTTPLIB_REV(N) case http::status::N: return status::N;
    constexpr status
    to_status(http::status v) noexcept
    {
        switch (v)
        {
            HTTPLIB_STATUS_TABLE(HTTPLIB_REV)
        }
        return status::unknown;
    }
#undef HTTPLIB_REV

    // ---- 头字段 ----

    /// field -> 线上字段名；unknown 与枚举外值返回空。
    constexpr std::string_view
    field_name(field v) noexcept
    {
        switch (v)
        {
#define HTTPLIB_FIELD_CASE(N, S) case field::N: return S;
            HTTPLIB_FIELD_TABLE(HTTPLIB_FIELD_CASE)
#undef HTTPLIB_FIELD_CASE
        }
        return {};
    }

    namespace detail
    {
        /// Beast 的 iequals 只做低 ASCII 大小写折叠，正是 HTTP 字段名语义。
        inline bool
        field_name_equals(std::string_view a, std::string_view b) noexcept
        {
            return beast::iequals(beast::string_view(a.data(), a.size()),
                                  beast::string_view(b.data(), b.size()));
        }
    } // namespace detail

    /// 线上字段名 -> field，按 HTTP 的大小写不敏感规则匹配；未知返回 unknown。
    inline field
    field_from_name(std::string_view name) noexcept
    {
        if (name.empty())
        {
            return field::unknown;
        }

#define HTTPLIB_FIELD_MATCH(N, S) \
    if (detail::field_name_equals(name, S)) \
    { \
        return field::N; \
    }
        HTTPLIB_FIELD_TABLE(HTTPLIB_FIELD_MATCH)
#undef HTTPLIB_FIELD_MATCH
        return field::unknown;
    }

#undef HTTPLIB_VERB_TABLE
#undef HTTPLIB_STATUS_TABLE
#undef HTTPLIB_FIELD_TABLE

    // ---- 枚举外数值兜底 ----
    //
    // method / status 各方向的末尾 return 只能由枚举外的数值抵达（枚举内的值全部
    // 由表展开出 case），故这批断言锁的是兜底分支本身，不是逐值映射。
    static_assert(to_verb(static_cast<httplib::method>(9999)) == http::verb::unknown);
    static_assert(to_method(static_cast<http::verb>(9999)) == httplib::method::unknown);
    static_assert(to_status(static_cast<httplib::status>(9999)) == http::status::unknown);
    static_assert(to_status(static_cast<http::status>(9999)) == httplib::status::unknown);
    static_assert(field_name(field::content_type) == "Content-Type");
} // namespace httplib::enum_conv
