#pragma once

// 公共的 httplib::{method,status,field} 与 beast 的 http::{verb,status,field} 之间
// 唯一的转换入口。本头是 lib 私有头（include/httplib/ 里不能出现 beast）。
//
// 转换一律走全覆盖 constexpr switch，勿用 static_cast：两套枚举各自独立声明，
// static_cast 在值失配时照样编译通过，而这三个枚举直接决定线上字节，错一位就是协议
// 破坏且只在运行时暴露。不写 default，末尾单独 return 兜底，枚举外的数值退回 unknown
// 而不是 UB。
//
// 转换按名字映射，不依赖两套枚举数值相同——enums.hpp 里的数值从不直接上线
// （status 是真实 HTTP 码，beast 是顺序值）。
//
// ---- 名字表 ----
//
// 每个名字只列一次。下述三张 X-macro 表展开出正向 / 反向两组 switch，两个方向因此
// 不可能各自漂移；加一个枚举值 = 在表里加一行，不用再翻 4 处。
//
// 表项恒为同名对同名（tests/enum_conv_test.cpp 会校验两套枚举逐名相同）。若将来出现
// 异名映射，把表项改成 X(本库名, beast 名) 并同步展开宏的参数解构即可。
//
// 覆盖性由表本身保证：表里有的名字两个方向都有 case，故「漏映射」不再可能——这也是
// 原来那 908 条逐值 static_assert 可以删掉的原因：它们与 case 同源，已经是恒真的
// 重述，不再提供任何信息。真正剩下的唯一风险是 enums.hpp 加了枚举值却忘了在表里
// 加行，该值会静默退回 unknown，由 enum_conv_test 兜住。
//
// 表区域关掉 clang-format：续行反斜杠的列对齐靠上面手工维护，格式化会把它搅乱。

#include "httplib/config.hpp"
#include "httplib/enums.hpp"

#include "beast_alias.hpp"

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
    X(unknown)                                                                                     \
    X(accept)                                                                                      \
    X(accept_ch)                                                                                   \
    X(accept_charset)                                                                              \
    X(accept_encoding)                                                                             \
    X(accept_language)                                                                             \
    X(accept_patch)                                                                                \
    X(accept_post)                                                                                 \
    X(accept_ranges)                                                                               \
    X(accept_signature)                                                                            \
    X(access_control_allow_credentials)                                                            \
    X(access_control_allow_headers)                                                                \
    X(access_control_allow_methods)                                                                \
    X(access_control_allow_origin)                                                                 \
    X(access_control_expose_headers)                                                               \
    X(access_control_max_age)                                                                      \
    X(access_control_request_headers)                                                              \
    X(access_control_request_method)                                                               \
    X(age)                                                                                         \
    X(allow)                                                                                       \
    X(alt_svc)                                                                                     \
    X(alt_used)                                                                                    \
    X(authorization)                                                                               \
    X(cache_control)                                                                               \
    X(clear_site_data)                                                                             \
    X(connection)                                                                                  \
    X(content_digest)                                                                              \
    X(content_disposition)                                                                         \
    X(content_dpr)                                                                                 \
    X(content_encoding)                                                                            \
    X(content_language)                                                                            \
    X(content_length)                                                                              \
    X(content_location)                                                                            \
    X(content_range)                                                                               \
    X(content_security_policy)                                                                     \
    X(content_security_policy_report_only)                                                         \
    X(content_type)                                                                                \
    X(cookie)                                                                                      \
    X(cross_origin_embedder_policy)                                                                \
    X(cross_origin_opener_policy)                                                                  \
    X(cross_origin_resource_policy)                                                                \
    X(date)                                                                                        \
    X(deprecation)                                                                                 \
    X(device_memory)                                                                               \
    X(digest)                                                                                      \
    X(dnt)                                                                                         \
    X(dpr)                                                                                         \
    X(etag)                                                                                        \
    X(expect)                                                                                      \
    X(expect_ct)                                                                                   \
    X(expires)                                                                                     \
    X(forwarded)                                                                                   \
    X(from)                                                                                        \
    X(host)                                                                                        \
    X(http2_settings)                                                                              \
    X(if_match)                                                                                    \
    X(if_modified_since)                                                                           \
    X(if_none_match)                                                                               \
    X(if_range)                                                                                    \
    X(if_unmodified_since)                                                                         \
    X(keep_alive)                                                                                  \
    X(last_modified)                                                                               \
    X(link)                                                                                        \
    X(location)                                                                                    \
    X(max_forwards)                                                                                \
    X(origin)                                                                                      \
    X(origin_agent_cluster)                                                                        \
    X(pragma)                                                                                      \
    X(prefer)                                                                                      \
    X(preference_applied)                                                                          \
    X(priority)                                                                                    \
    X(proxy_authenticate)                                                                          \
    X(proxy_authorization)                                                                         \
    X(proxy_connection)                                                                            \
    X(range)                                                                                       \
    X(referer)                                                                                     \
    X(referrer_policy)                                                                             \
    X(refresh)                                                                                     \
    X(report_to)                                                                                   \
    X(reporting_endpoints)                                                                         \
    X(repr_digest)                                                                                 \
    X(retry_after)                                                                                 \
    X(sec_ch_ua_full_version)                                                                      \
    X(sec_fetch_dest)                                                                              \
    X(sec_fetch_mode)                                                                              \
    X(sec_fetch_site)                                                                              \
    X(sec_fetch_user)                                                                              \
    X(sec_purpose)                                                                                 \
    X(sec_websocket_accept)                                                                        \
    X(sec_websocket_extensions)                                                                    \
    X(sec_websocket_key)                                                                           \
    X(sec_websocket_protocol)                                                                      \
    X(sec_websocket_version)                                                                       \
    X(server)                                                                                      \
    X(server_timing)                                                                               \
    X(service_worker)                                                                              \
    X(service_worker_allowed)                                                                      \
    X(service_worker_navigation_preload)                                                           \
    X(set_cookie)                                                                                  \
    X(set_login)                                                                                   \
    X(signature)                                                                                   \
    X(signature_input)                                                                             \
    X(sourcemap)                                                                                   \
    X(strict_transport_security)                                                                   \
    X(te)                                                                                          \
    X(timing_allow_origin)                                                                         \
    X(tk)                                                                                          \
    X(trailer)                                                                                     \
    X(transfer_encoding)                                                                           \
    X(upgrade)                                                                                     \
    X(upgrade_insecure_requests)                                                                   \
    X(user_agent)                                                                                  \
    X(vary)                                                                                        \
    X(via)                                                                                         \
    X(viewport_width)                                                                              \
    X(want_content_digest)                                                                         \
    X(want_repr_digest)                                                                            \
    X(warning)                                                                                     \
    X(width)                                                                                       \
    X(www_authenticate)                                                                            \
    X(x_content_type_options)                                                                      \
    X(x_dns_prefetch_control)                                                                      \
    X(x_forwarded_for)                                                                             \
    X(x_forwarded_host)                                                                            \
    X(x_forwarded_proto)                                                                           \
    X(x_frame_options)                                                                             \
    X(x_permitted_cross_domain_policies)                                                           \
    X(x_powered_by)                                                                                \
    X(x_robots_tag)                                                                                \
    X(x_xss_protection)

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

    /// field -> http::field
#define HTTPLIB_FWD(N) case field::N: return http::field::N;
    constexpr http::field
    to_field(field v) noexcept
    {
        switch (v)
        {
            HTTPLIB_FIELD_TABLE(HTTPLIB_FWD)
        }
        return http::field::unknown;
    }
#undef HTTPLIB_FWD

    /// http::field -> field
#define HTTPLIB_REV(N) case http::field::N: return field::N;
    constexpr field
    to_field(http::field v) noexcept
    {
        switch (v)
        {
            HTTPLIB_FIELD_TABLE(HTTPLIB_REV)
        }
        return field::unknown;
    }
#undef HTTPLIB_REV

#undef HTTPLIB_VERB_TABLE
#undef HTTPLIB_STATUS_TABLE
#undef HTTPLIB_FIELD_TABLE

    // ---- 枚举外数值兜底 ----
    //
    // 六个方向的末尾 return 只能由枚举外的数值抵达（枚举内的值全部由表展开出 case），
    // 故这批断言锁的是兜底分支本身，不是逐值映射。
    static_assert(to_verb(static_cast<httplib::method>(9999)) == http::verb::unknown);
    static_assert(to_method(static_cast<http::verb>(9999)) == httplib::method::unknown);
    static_assert(to_status(static_cast<httplib::status>(9999)) == http::status::unknown);
    static_assert(to_status(static_cast<http::status>(9999)) == httplib::status::unknown);
    static_assert(to_field(static_cast<httplib::field>(9999)) == http::field::unknown);
    static_assert(to_field(static_cast<http::field>(9999)) == httplib::field::unknown);
} // namespace httplib::enum_conv
