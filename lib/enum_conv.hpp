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
    X(a_im)                                                                                        \
    X(accept)                                                                                      \
    X(accept_additions)                                                                            \
    X(accept_charset)                                                                              \
    X(accept_datetime)                                                                             \
    X(accept_encoding)                                                                             \
    X(accept_features)                                                                             \
    X(accept_language)                                                                             \
    X(accept_patch)                                                                                \
    X(accept_post)                                                                                 \
    X(accept_ranges)                                                                               \
    X(access_control)                                                                              \
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
    X(alpn)                                                                                        \
    X(also_control)                                                                                \
    X(alt_svc)                                                                                     \
    X(alt_used)                                                                                    \
    X(alternate_recipient)                                                                         \
    X(alternates)                                                                                  \
    X(apparently_to)                                                                               \
    X(apply_to_redirect_ref)                                                                       \
    X(approved)                                                                                    \
    X(archive)                                                                                     \
    X(archived_at)                                                                                 \
    X(article_names)                                                                               \
    X(article_updates)                                                                             \
    X(authentication_control)                                                                      \
    X(authentication_info)                                                                         \
    X(authentication_results)                                                                      \
    X(authorization)                                                                               \
    X(auto_submitted)                                                                              \
    X(autoforwarded)                                                                               \
    X(autosubmitted)                                                                               \
    X(base)                                                                                        \
    X(bcc)                                                                                         \
    X(body)                                                                                        \
    X(c_ext)                                                                                       \
    X(c_man)                                                                                       \
    X(c_opt)                                                                                       \
    X(c_pep)                                                                                       \
    X(c_pep_info)                                                                                  \
    X(cache_control)                                                                               \
    X(caldav_timezones)                                                                            \
    X(cancel_key)                                                                                  \
    X(cancel_lock)                                                                                 \
    X(cc)                                                                                          \
    X(close)                                                                                       \
    X(comments)                                                                                    \
    X(compliance)                                                                                  \
    X(connection)                                                                                  \
    X(content_alternative)                                                                         \
    X(content_base)                                                                                \
    X(content_description)                                                                         \
    X(content_disposition)                                                                         \
    X(content_duration)                                                                            \
    X(content_encoding)                                                                            \
    X(content_features)                                                                            \
    X(content_id)                                                                                  \
    X(content_identifier)                                                                          \
    X(content_language)                                                                            \
    X(content_length)                                                                              \
    X(content_location)                                                                            \
    X(content_md5)                                                                                 \
    X(content_range)                                                                               \
    X(content_return)                                                                              \
    X(content_script_type)                                                                         \
    X(content_style_type)                                                                          \
    X(content_transfer_encoding)                                                                   \
    X(content_type)                                                                                \
    X(content_version)                                                                             \
    X(control)                                                                                     \
    X(conversion)                                                                                  \
    X(conversion_with_loss)                                                                        \
    X(cookie)                                                                                      \
    X(cookie2)                                                                                     \
    X(cost)                                                                                        \
    X(dasl)                                                                                        \
    X(date)                                                                                        \
    X(date_received)                                                                               \
    X(dav)                                                                                         \
    X(default_style)                                                                               \
    X(deferred_delivery)                                                                           \
    X(delivery_date)                                                                               \
    X(delta_base)                                                                                  \
    X(depth)                                                                                       \
    X(derived_from)                                                                                \
    X(destination)                                                                                 \
    X(differential_id)                                                                             \
    X(digest)                                                                                      \
    X(discarded_x400_ipms_extensions)                                                              \
    X(discarded_x400_mts_extensions)                                                               \
    X(disclose_recipients)                                                                         \
    X(disposition_notification_options)                                                            \
    X(disposition_notification_to)                                                                 \
    X(distribution)                                                                                \
    X(dkim_signature)                                                                              \
    X(dl_expansion_history)                                                                        \
    X(downgraded_bcc)                                                                              \
    X(downgraded_cc)                                                                               \
    X(downgraded_disposition_notification_to)                                                      \
    X(downgraded_final_recipient)                                                                  \
    X(downgraded_from)                                                                             \
    X(downgraded_in_reply_to)                                                                      \
    X(downgraded_mail_from)                                                                        \
    X(downgraded_message_id)                                                                       \
    X(downgraded_original_recipient)                                                               \
    X(downgraded_rcpt_to)                                                                          \
    X(downgraded_references)                                                                       \
    X(downgraded_reply_to)                                                                         \
    X(downgraded_resent_bcc)                                                                       \
    X(downgraded_resent_cc)                                                                        \
    X(downgraded_resent_from)                                                                      \
    X(downgraded_resent_reply_to)                                                                  \
    X(downgraded_resent_sender)                                                                    \
    X(downgraded_resent_to)                                                                        \
    X(downgraded_return_path)                                                                      \
    X(downgraded_sender)                                                                           \
    X(downgraded_to)                                                                               \
    X(ediint_features)                                                                             \
    X(eesst_version)                                                                               \
    X(encoding)                                                                                    \
    X(encrypted)                                                                                   \
    X(errors_to)                                                                                   \
    X(etag)                                                                                        \
    X(expect)                                                                                      \
    X(expires)                                                                                     \
    X(expiry_date)                                                                                 \
    X(ext)                                                                                         \
    X(followup_to)                                                                                 \
    X(forwarded)                                                                                   \
    X(from)                                                                                        \
    X(generate_delivery_report)                                                                    \
    X(getprofile)                                                                                  \
    X(hobareg)                                                                                     \
    X(host)                                                                                        \
    X(http2_settings)                                                                              \
    X(if_)                                                                                         \
    X(if_match)                                                                                    \
    X(if_modified_since)                                                                           \
    X(if_none_match)                                                                               \
    X(if_range)                                                                                    \
    X(if_schedule_tag_match)                                                                       \
    X(if_unmodified_since)                                                                         \
    X(im)                                                                                          \
    X(importance)                                                                                  \
    X(in_reply_to)                                                                                 \
    X(incomplete_copy)                                                                             \
    X(injection_date)                                                                              \
    X(injection_info)                                                                              \
    X(jabber_id)                                                                                   \
    X(keep_alive)                                                                                  \
    X(keywords)                                                                                    \
    X(label)                                                                                       \
    X(language)                                                                                    \
    X(last_modified)                                                                               \
    X(latest_delivery_time)                                                                        \
    X(lines)                                                                                       \
    X(link)                                                                                        \
    X(list_archive)                                                                                \
    X(list_help)                                                                                   \
    X(list_id)                                                                                     \
    X(list_owner)                                                                                  \
    X(list_post)                                                                                   \
    X(list_subscribe)                                                                              \
    X(list_unsubscribe)                                                                            \
    X(list_unsubscribe_post)                                                                       \
    X(location)                                                                                    \
    X(lock_token)                                                                                  \
    X(man)                                                                                         \
    X(max_forwards)                                                                                \
    X(memento_datetime)                                                                            \
    X(message_context)                                                                             \
    X(message_id)                                                                                  \
    X(message_type)                                                                                \
    X(meter)                                                                                       \
    X(method_check)                                                                                \
    X(method_check_expires)                                                                        \
    X(mime_version)                                                                                \
    X(mmhs_acp127_message_identifier)                                                              \
    X(mmhs_authorizing_users)                                                                      \
    X(mmhs_codress_message_indicator)                                                              \
    X(mmhs_copy_precedence)                                                                        \
    X(mmhs_exempted_address)                                                                       \
    X(mmhs_extended_authorisation_info)                                                            \
    X(mmhs_handling_instructions)                                                                  \
    X(mmhs_message_instructions)                                                                   \
    X(mmhs_message_type)                                                                           \
    X(mmhs_originator_plad)                                                                        \
    X(mmhs_originator_reference)                                                                   \
    X(mmhs_other_recipients_indicator_cc)                                                          \
    X(mmhs_other_recipients_indicator_to)                                                          \
    X(mmhs_primary_precedence)                                                                     \
    X(mmhs_subject_indicator_codes)                                                                \
    X(mt_priority)                                                                                 \
    X(negotiate)                                                                                   \
    X(newsgroups)                                                                                  \
    X(nntp_posting_date)                                                                           \
    X(nntp_posting_host)                                                                           \
    X(non_compliance)                                                                              \
    X(obsoletes)                                                                                   \
    X(opt)                                                                                         \
    X(optional)                                                                                    \
    X(optional_www_authenticate)                                                                   \
    X(ordering_type)                                                                               \
    X(organization)                                                                                \
    X(origin)                                                                                      \
    X(original_encoded_information_types)                                                          \
    X(original_from)                                                                               \
    X(original_message_id)                                                                         \
    X(original_recipient)                                                                          \
    X(original_sender)                                                                             \
    X(original_subject)                                                                            \
    X(originator_return_address)                                                                   \
    X(overwrite)                                                                                   \
    X(p3p)                                                                                         \
    X(path)                                                                                        \
    X(pep)                                                                                         \
    X(pep_info)                                                                                    \
    X(pics_label)                                                                                  \
    X(position)                                                                                    \
    X(posting_version)                                                                             \
    X(pragma)                                                                                      \
    X(prefer)                                                                                      \
    X(preference_applied)                                                                          \
    X(prevent_nondelivery_report)                                                                  \
    X(priority)                                                                                    \
    X(privicon)                                                                                    \
    X(profileobject)                                                                               \
    X(protocol)                                                                                    \
    X(protocol_info)                                                                               \
    X(protocol_query)                                                                              \
    X(protocol_request)                                                                            \
    X(proxy_authenticate)                                                                          \
    X(proxy_authentication_info)                                                                   \
    X(proxy_authorization)                                                                         \
    X(proxy_connection)                                                                            \
    X(proxy_features)                                                                              \
    X(proxy_instruction)                                                                           \
    X(public_)                                                                                     \
    X(public_key_pins)                                                                             \
    X(public_key_pins_report_only)                                                                 \
    X(range)                                                                                       \
    X(received)                                                                                    \
    X(received_spf)                                                                                \
    X(redirect_ref)                                                                                \
    X(references)                                                                                  \
    X(referer)                                                                                     \
    X(referer_root)                                                                                \
    X(relay_version)                                                                               \
    X(reply_by)                                                                                    \
    X(reply_to)                                                                                    \
    X(require_recipient_valid_since)                                                               \
    X(resent_bcc)                                                                                  \
    X(resent_cc)                                                                                   \
    X(resent_date)                                                                                 \
    X(resent_from)                                                                                 \
    X(resent_message_id)                                                                           \
    X(resent_reply_to)                                                                             \
    X(resent_sender)                                                                               \
    X(resent_to)                                                                                   \
    X(resolution_hint)                                                                             \
    X(resolver_location)                                                                           \
    X(retry_after)                                                                                 \
    X(return_path)                                                                                 \
    X(safe)                                                                                        \
    X(schedule_reply)                                                                              \
    X(schedule_tag)                                                                                \
    X(sec_fetch_dest)                                                                              \
    X(sec_fetch_mode)                                                                              \
    X(sec_fetch_site)                                                                              \
    X(sec_fetch_user)                                                                              \
    X(sec_websocket_accept)                                                                        \
    X(sec_websocket_extensions)                                                                    \
    X(sec_websocket_key)                                                                           \
    X(sec_websocket_protocol)                                                                      \
    X(sec_websocket_version)                                                                       \
    X(security_scheme)                                                                             \
    X(see_also)                                                                                    \
    X(sender)                                                                                      \
    X(sensitivity)                                                                                 \
    X(server)                                                                                      \
    X(set_cookie)                                                                                  \
    X(set_cookie2)                                                                                 \
    X(setprofile)                                                                                  \
    X(sio_label)                                                                                   \
    X(sio_label_history)                                                                           \
    X(slug)                                                                                        \
    X(soapaction)                                                                                  \
    X(solicitation)                                                                                \
    X(status_uri)                                                                                  \
    X(strict_transport_security)                                                                   \
    X(subject)                                                                                     \
    X(subok)                                                                                       \
    X(subst)                                                                                       \
    X(summary)                                                                                     \
    X(supersedes)                                                                                  \
    X(surrogate_capability)                                                                        \
    X(surrogate_control)                                                                           \
    X(tcn)                                                                                         \
    X(te)                                                                                          \
    X(timeout)                                                                                     \
    X(title)                                                                                       \
    X(to)                                                                                          \
    X(topic)                                                                                       \
    X(trailer)                                                                                     \
    X(transfer_encoding)                                                                           \
    X(ttl)                                                                                         \
    X(ua_color)                                                                                    \
    X(ua_media)                                                                                    \
    X(ua_pixels)                                                                                   \
    X(ua_resolution)                                                                               \
    X(ua_windowpixels)                                                                             \
    X(upgrade)                                                                                     \
    X(urgency)                                                                                     \
    X(uri)                                                                                         \
    X(user_agent)                                                                                  \
    X(variant_vary)                                                                                \
    X(vary)                                                                                        \
    X(vbr_info)                                                                                    \
    X(version)                                                                                     \
    X(via)                                                                                         \
    X(want_digest)                                                                                 \
    X(warning)                                                                                     \
    X(www_authenticate)                                                                            \
    X(x_archived_at)                                                                               \
    X(x_device_accept)                                                                             \
    X(x_device_accept_charset)                                                                     \
    X(x_device_accept_encoding)                                                                    \
    X(x_device_accept_language)                                                                    \
    X(x_device_user_agent)                                                                         \
    X(x_frame_options)                                                                             \
    X(x_mittente)                                                                                  \
    X(x_pgp_sig)                                                                                   \
    X(x_ricevuta)                                                                                  \
    X(x_riferimento_message_id)                                                                    \
    X(x_tiporicevuta)                                                                              \
    X(x_trasporto)                                                                                 \
    X(x_verificasicurezza)                                                                         \
    X(x400_content_identifier)                                                                     \
    X(x400_content_return)                                                                         \
    X(x400_content_type)                                                                           \
    X(x400_mts_identifier)                                                                         \
    X(x400_originator)                                                                             \
    X(x400_received)                                                                               \
    X(x400_recipients)                                                                             \
    X(x400_trace)                                                                                  \
    X(xref)

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
