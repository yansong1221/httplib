#pragma once

// 公共的 httplib::{method,status,field} 与 beast 的 http::{verb,status,field} 之间
// 唯一的转换入口。本头是 lib 私有头（include/httplib/ 里不能出现 beast）。
//
// 转换一律走全覆盖 constexpr switch，勿用 static_cast：两套枚举各自独立声明，
// static_cast 在值失配时照样编译通过，而这三个枚举直接决定线上字节，错一位就是协议
// 破坏且只在运行时暴露。不写 default，末尾单独 return 兜底，枚举外的数值退回 unknown
// 而不是 UB。
//
// 兜底 return 意味着漏掉的枚举值只会静默退回 unknown，所以文件末尾对每个枚举值逐个
// static_assert，漏映射 / 错位直接编译失败。断言放本头（实测编译时间无差异）。
//
// 转换按名字映射，不依赖两套枚举数值相同——enums.hpp 里的数值从不直接上线。

#include "httplib/config.hpp"
#include "httplib/enums.hpp"

#include "beast_alias.hpp"

namespace httplib::enum_conv
{

    /// method -> http::verb
    constexpr http::verb
    to_verb(method v) noexcept
    {
        switch (v)
        {
            case method::unknown:
                return http::verb::unknown;
            case method::delete_:
                return http::verb::delete_;
            case method::get:
                return http::verb::get;
            case method::head:
                return http::verb::head;
            case method::post:
                return http::verb::post;
            case method::put:
                return http::verb::put;
            case method::connect:
                return http::verb::connect;
            case method::options:
                return http::verb::options;
            case method::trace:
                return http::verb::trace;
            case method::copy:
                return http::verb::copy;
            case method::lock:
                return http::verb::lock;
            case method::mkcol:
                return http::verb::mkcol;
            case method::move:
                return http::verb::move;
            case method::propfind:
                return http::verb::propfind;
            case method::proppatch:
                return http::verb::proppatch;
            case method::search:
                return http::verb::search;
            case method::unlock:
                return http::verb::unlock;
            case method::bind:
                return http::verb::bind;
            case method::rebind:
                return http::verb::rebind;
            case method::unbind:
                return http::verb::unbind;
            case method::acl:
                return http::verb::acl;
            case method::report:
                return http::verb::report;
            case method::mkactivity:
                return http::verb::mkactivity;
            case method::checkout:
                return http::verb::checkout;
            case method::merge:
                return http::verb::merge;
            case method::msearch:
                return http::verb::msearch;
            case method::notify:
                return http::verb::notify;
            case method::subscribe:
                return http::verb::subscribe;
            case method::unsubscribe:
                return http::verb::unsubscribe;
            case method::patch:
                return http::verb::patch;
            case method::purge:
                return http::verb::purge;
            case method::mkcalendar:
                return http::verb::mkcalendar;
            case method::link:
                return http::verb::link;
            case method::unlink:
                return http::verb::unlink;
        }

        // 枚举外的数值（如 static_cast<method>(数字) 构造的未知码）退回 unknown，
        return http::verb::unknown;
    }

    /// http::verb -> method
    constexpr method
    to_method(http::verb v) noexcept
    {
        switch (v)
        {
            case http::verb::unknown:
                return method::unknown;
            case http::verb::delete_:
                return method::delete_;
            case http::verb::get:
                return method::get;
            case http::verb::head:
                return method::head;
            case http::verb::post:
                return method::post;
            case http::verb::put:
                return method::put;
            case http::verb::connect:
                return method::connect;
            case http::verb::options:
                return method::options;
            case http::verb::trace:
                return method::trace;
            case http::verb::copy:
                return method::copy;
            case http::verb::lock:
                return method::lock;
            case http::verb::mkcol:
                return method::mkcol;
            case http::verb::move:
                return method::move;
            case http::verb::propfind:
                return method::propfind;
            case http::verb::proppatch:
                return method::proppatch;
            case http::verb::search:
                return method::search;
            case http::verb::unlock:
                return method::unlock;
            case http::verb::bind:
                return method::bind;
            case http::verb::rebind:
                return method::rebind;
            case http::verb::unbind:
                return method::unbind;
            case http::verb::acl:
                return method::acl;
            case http::verb::report:
                return method::report;
            case http::verb::mkactivity:
                return method::mkactivity;
            case http::verb::checkout:
                return method::checkout;
            case http::verb::merge:
                return method::merge;
            case http::verb::msearch:
                return method::msearch;
            case http::verb::notify:
                return method::notify;
            case http::verb::subscribe:
                return method::subscribe;
            case http::verb::unsubscribe:
                return method::unsubscribe;
            case http::verb::patch:
                return method::patch;
            case http::verb::purge:
                return method::purge;
            case http::verb::mkcalendar:
                return method::mkcalendar;
            case http::verb::link:
                return method::link;
            case http::verb::unlink:
                return method::unlink;
        }

        // 枚举外的数值（如 static_cast<http::verb>(数字) 构造的未知码）退回 unknown，
        return method::unknown;
    }

    /// status -> http::status
    constexpr http::status
    to_status(status v) noexcept
    {
        switch (v)
        {
            case status::unknown:
                return http::status::unknown;
            case status::continue_:
                return http::status::continue_;
            case status::switching_protocols:
                return http::status::switching_protocols;
            case status::processing:
                return http::status::processing;
            case status::early_hints:
                return http::status::early_hints;
            case status::ok:
                return http::status::ok;
            case status::created:
                return http::status::created;
            case status::accepted:
                return http::status::accepted;
            case status::non_authoritative_information:
                return http::status::non_authoritative_information;
            case status::no_content:
                return http::status::no_content;
            case status::reset_content:
                return http::status::reset_content;
            case status::partial_content:
                return http::status::partial_content;
            case status::multi_status:
                return http::status::multi_status;
            case status::already_reported:
                return http::status::already_reported;
            case status::im_used:
                return http::status::im_used;
            case status::multiple_choices:
                return http::status::multiple_choices;
            case status::moved_permanently:
                return http::status::moved_permanently;
            case status::found:
                return http::status::found;
            case status::see_other:
                return http::status::see_other;
            case status::not_modified:
                return http::status::not_modified;
            case status::use_proxy:
                return http::status::use_proxy;
            case status::temporary_redirect:
                return http::status::temporary_redirect;
            case status::permanent_redirect:
                return http::status::permanent_redirect;
            case status::bad_request:
                return http::status::bad_request;
            case status::unauthorized:
                return http::status::unauthorized;
            case status::payment_required:
                return http::status::payment_required;
            case status::forbidden:
                return http::status::forbidden;
            case status::not_found:
                return http::status::not_found;
            case status::method_not_allowed:
                return http::status::method_not_allowed;
            case status::not_acceptable:
                return http::status::not_acceptable;
            case status::proxy_authentication_required:
                return http::status::proxy_authentication_required;
            case status::request_timeout:
                return http::status::request_timeout;
            case status::conflict:
                return http::status::conflict;
            case status::gone:
                return http::status::gone;
            case status::length_required:
                return http::status::length_required;
            case status::precondition_failed:
                return http::status::precondition_failed;
            case status::payload_too_large:
                return http::status::payload_too_large;
            case status::uri_too_long:
                return http::status::uri_too_long;
            case status::unsupported_media_type:
                return http::status::unsupported_media_type;
            case status::range_not_satisfiable:
                return http::status::range_not_satisfiable;
            case status::expectation_failed:
                return http::status::expectation_failed;
            case status::i_am_a_teapot:
                return http::status::i_am_a_teapot;
            case status::misdirected_request:
                return http::status::misdirected_request;
            case status::unprocessable_entity:
                return http::status::unprocessable_entity;
            case status::locked:
                return http::status::locked;
            case status::failed_dependency:
                return http::status::failed_dependency;
            case status::too_early:
                return http::status::too_early;
            case status::upgrade_required:
                return http::status::upgrade_required;
            case status::precondition_required:
                return http::status::precondition_required;
            case status::too_many_requests:
                return http::status::too_many_requests;
            case status::request_header_fields_too_large:
                return http::status::request_header_fields_too_large;
            case status::unavailable_for_legal_reasons:
                return http::status::unavailable_for_legal_reasons;
            case status::internal_server_error:
                return http::status::internal_server_error;
            case status::not_implemented:
                return http::status::not_implemented;
            case status::bad_gateway:
                return http::status::bad_gateway;
            case status::service_unavailable:
                return http::status::service_unavailable;
            case status::gateway_timeout:
                return http::status::gateway_timeout;
            case status::http_version_not_supported:
                return http::status::http_version_not_supported;
            case status::variant_also_negotiates:
                return http::status::variant_also_negotiates;
            case status::insufficient_storage:
                return http::status::insufficient_storage;
            case status::loop_detected:
                return http::status::loop_detected;
            case status::not_extended:
                return http::status::not_extended;
            case status::network_authentication_required:
                return http::status::network_authentication_required;
        }

        // 枚举外的数值（如 static_cast<status>(数字) 构造的未知码）退回 unknown，
        return http::status::unknown;
    }

    /// http::status -> status
    constexpr status
    to_status(http::status v) noexcept
    {
        switch (v)
        {
            case http::status::unknown:
                return status::unknown;
            case http::status::continue_:
                return status::continue_;
            case http::status::switching_protocols:
                return status::switching_protocols;
            case http::status::processing:
                return status::processing;
            case http::status::early_hints:
                return status::early_hints;
            case http::status::ok:
                return status::ok;
            case http::status::created:
                return status::created;
            case http::status::accepted:
                return status::accepted;
            case http::status::non_authoritative_information:
                return status::non_authoritative_information;
            case http::status::no_content:
                return status::no_content;
            case http::status::reset_content:
                return status::reset_content;
            case http::status::partial_content:
                return status::partial_content;
            case http::status::multi_status:
                return status::multi_status;
            case http::status::already_reported:
                return status::already_reported;
            case http::status::im_used:
                return status::im_used;
            case http::status::multiple_choices:
                return status::multiple_choices;
            case http::status::moved_permanently:
                return status::moved_permanently;
            case http::status::found:
                return status::found;
            case http::status::see_other:
                return status::see_other;
            case http::status::not_modified:
                return status::not_modified;
            case http::status::use_proxy:
                return status::use_proxy;
            case http::status::temporary_redirect:
                return status::temporary_redirect;
            case http::status::permanent_redirect:
                return status::permanent_redirect;
            case http::status::bad_request:
                return status::bad_request;
            case http::status::unauthorized:
                return status::unauthorized;
            case http::status::payment_required:
                return status::payment_required;
            case http::status::forbidden:
                return status::forbidden;
            case http::status::not_found:
                return status::not_found;
            case http::status::method_not_allowed:
                return status::method_not_allowed;
            case http::status::not_acceptable:
                return status::not_acceptable;
            case http::status::proxy_authentication_required:
                return status::proxy_authentication_required;
            case http::status::request_timeout:
                return status::request_timeout;
            case http::status::conflict:
                return status::conflict;
            case http::status::gone:
                return status::gone;
            case http::status::length_required:
                return status::length_required;
            case http::status::precondition_failed:
                return status::precondition_failed;
            case http::status::payload_too_large:
                return status::payload_too_large;
            case http::status::uri_too_long:
                return status::uri_too_long;
            case http::status::unsupported_media_type:
                return status::unsupported_media_type;
            case http::status::range_not_satisfiable:
                return status::range_not_satisfiable;
            case http::status::expectation_failed:
                return status::expectation_failed;
            case http::status::i_am_a_teapot:
                return status::i_am_a_teapot;
            case http::status::misdirected_request:
                return status::misdirected_request;
            case http::status::unprocessable_entity:
                return status::unprocessable_entity;
            case http::status::locked:
                return status::locked;
            case http::status::failed_dependency:
                return status::failed_dependency;
            case http::status::too_early:
                return status::too_early;
            case http::status::upgrade_required:
                return status::upgrade_required;
            case http::status::precondition_required:
                return status::precondition_required;
            case http::status::too_many_requests:
                return status::too_many_requests;
            case http::status::request_header_fields_too_large:
                return status::request_header_fields_too_large;
            case http::status::unavailable_for_legal_reasons:
                return status::unavailable_for_legal_reasons;
            case http::status::internal_server_error:
                return status::internal_server_error;
            case http::status::not_implemented:
                return status::not_implemented;
            case http::status::bad_gateway:
                return status::bad_gateway;
            case http::status::service_unavailable:
                return status::service_unavailable;
            case http::status::gateway_timeout:
                return status::gateway_timeout;
            case http::status::http_version_not_supported:
                return status::http_version_not_supported;
            case http::status::variant_also_negotiates:
                return status::variant_also_negotiates;
            case http::status::insufficient_storage:
                return status::insufficient_storage;
            case http::status::loop_detected:
                return status::loop_detected;
            case http::status::not_extended:
                return status::not_extended;
            case http::status::network_authentication_required:
                return status::network_authentication_required;
        }

        // 枚举外的数值（如 static_cast<http::status>(数字) 构造的未知码）退回 unknown，
        return status::unknown;
    }

    /// field -> http::field
    constexpr http::field
    to_field(field v) noexcept
    {
        switch (v)
        {
            case field::unknown:
                return http::field::unknown;
            case field::a_im:
                return http::field::a_im;
            case field::accept:
                return http::field::accept;
            case field::accept_additions:
                return http::field::accept_additions;
            case field::accept_charset:
                return http::field::accept_charset;
            case field::accept_datetime:
                return http::field::accept_datetime;
            case field::accept_encoding:
                return http::field::accept_encoding;
            case field::accept_features:
                return http::field::accept_features;
            case field::accept_language:
                return http::field::accept_language;
            case field::accept_patch:
                return http::field::accept_patch;
            case field::accept_post:
                return http::field::accept_post;
            case field::accept_ranges:
                return http::field::accept_ranges;
            case field::access_control:
                return http::field::access_control;
            case field::access_control_allow_credentials:
                return http::field::access_control_allow_credentials;
            case field::access_control_allow_headers:
                return http::field::access_control_allow_headers;
            case field::access_control_allow_methods:
                return http::field::access_control_allow_methods;
            case field::access_control_allow_origin:
                return http::field::access_control_allow_origin;
            case field::access_control_expose_headers:
                return http::field::access_control_expose_headers;
            case field::access_control_max_age:
                return http::field::access_control_max_age;
            case field::access_control_request_headers:
                return http::field::access_control_request_headers;
            case field::access_control_request_method:
                return http::field::access_control_request_method;
            case field::age:
                return http::field::age;
            case field::allow:
                return http::field::allow;
            case field::alpn:
                return http::field::alpn;
            case field::also_control:
                return http::field::also_control;
            case field::alt_svc:
                return http::field::alt_svc;
            case field::alt_used:
                return http::field::alt_used;
            case field::alternate_recipient:
                return http::field::alternate_recipient;
            case field::alternates:
                return http::field::alternates;
            case field::apparently_to:
                return http::field::apparently_to;
            case field::apply_to_redirect_ref:
                return http::field::apply_to_redirect_ref;
            case field::approved:
                return http::field::approved;
            case field::archive:
                return http::field::archive;
            case field::archived_at:
                return http::field::archived_at;
            case field::article_names:
                return http::field::article_names;
            case field::article_updates:
                return http::field::article_updates;
            case field::authentication_control:
                return http::field::authentication_control;
            case field::authentication_info:
                return http::field::authentication_info;
            case field::authentication_results:
                return http::field::authentication_results;
            case field::authorization:
                return http::field::authorization;
            case field::auto_submitted:
                return http::field::auto_submitted;
            case field::autoforwarded:
                return http::field::autoforwarded;
            case field::autosubmitted:
                return http::field::autosubmitted;
            case field::base:
                return http::field::base;
            case field::bcc:
                return http::field::bcc;
            case field::body:
                return http::field::body;
            case field::c_ext:
                return http::field::c_ext;
            case field::c_man:
                return http::field::c_man;
            case field::c_opt:
                return http::field::c_opt;
            case field::c_pep:
                return http::field::c_pep;
            case field::c_pep_info:
                return http::field::c_pep_info;
            case field::cache_control:
                return http::field::cache_control;
            case field::caldav_timezones:
                return http::field::caldav_timezones;
            case field::cancel_key:
                return http::field::cancel_key;
            case field::cancel_lock:
                return http::field::cancel_lock;
            case field::cc:
                return http::field::cc;
            case field::close:
                return http::field::close;
            case field::comments:
                return http::field::comments;
            case field::compliance:
                return http::field::compliance;
            case field::connection:
                return http::field::connection;
            case field::content_alternative:
                return http::field::content_alternative;
            case field::content_base:
                return http::field::content_base;
            case field::content_description:
                return http::field::content_description;
            case field::content_disposition:
                return http::field::content_disposition;
            case field::content_duration:
                return http::field::content_duration;
            case field::content_encoding:
                return http::field::content_encoding;
            case field::content_features:
                return http::field::content_features;
            case field::content_id:
                return http::field::content_id;
            case field::content_identifier:
                return http::field::content_identifier;
            case field::content_language:
                return http::field::content_language;
            case field::content_length:
                return http::field::content_length;
            case field::content_location:
                return http::field::content_location;
            case field::content_md5:
                return http::field::content_md5;
            case field::content_range:
                return http::field::content_range;
            case field::content_return:
                return http::field::content_return;
            case field::content_script_type:
                return http::field::content_script_type;
            case field::content_style_type:
                return http::field::content_style_type;
            case field::content_transfer_encoding:
                return http::field::content_transfer_encoding;
            case field::content_type:
                return http::field::content_type;
            case field::content_version:
                return http::field::content_version;
            case field::control:
                return http::field::control;
            case field::conversion:
                return http::field::conversion;
            case field::conversion_with_loss:
                return http::field::conversion_with_loss;
            case field::cookie:
                return http::field::cookie;
            case field::cookie2:
                return http::field::cookie2;
            case field::cost:
                return http::field::cost;
            case field::dasl:
                return http::field::dasl;
            case field::date:
                return http::field::date;
            case field::date_received:
                return http::field::date_received;
            case field::dav:
                return http::field::dav;
            case field::default_style:
                return http::field::default_style;
            case field::deferred_delivery:
                return http::field::deferred_delivery;
            case field::delivery_date:
                return http::field::delivery_date;
            case field::delta_base:
                return http::field::delta_base;
            case field::depth:
                return http::field::depth;
            case field::derived_from:
                return http::field::derived_from;
            case field::destination:
                return http::field::destination;
            case field::differential_id:
                return http::field::differential_id;
            case field::digest:
                return http::field::digest;
            case field::discarded_x400_ipms_extensions:
                return http::field::discarded_x400_ipms_extensions;
            case field::discarded_x400_mts_extensions:
                return http::field::discarded_x400_mts_extensions;
            case field::disclose_recipients:
                return http::field::disclose_recipients;
            case field::disposition_notification_options:
                return http::field::disposition_notification_options;
            case field::disposition_notification_to:
                return http::field::disposition_notification_to;
            case field::distribution:
                return http::field::distribution;
            case field::dkim_signature:
                return http::field::dkim_signature;
            case field::dl_expansion_history:
                return http::field::dl_expansion_history;
            case field::downgraded_bcc:
                return http::field::downgraded_bcc;
            case field::downgraded_cc:
                return http::field::downgraded_cc;
            case field::downgraded_disposition_notification_to:
                return http::field::downgraded_disposition_notification_to;
            case field::downgraded_final_recipient:
                return http::field::downgraded_final_recipient;
            case field::downgraded_from:
                return http::field::downgraded_from;
            case field::downgraded_in_reply_to:
                return http::field::downgraded_in_reply_to;
            case field::downgraded_mail_from:
                return http::field::downgraded_mail_from;
            case field::downgraded_message_id:
                return http::field::downgraded_message_id;
            case field::downgraded_original_recipient:
                return http::field::downgraded_original_recipient;
            case field::downgraded_rcpt_to:
                return http::field::downgraded_rcpt_to;
            case field::downgraded_references:
                return http::field::downgraded_references;
            case field::downgraded_reply_to:
                return http::field::downgraded_reply_to;
            case field::downgraded_resent_bcc:
                return http::field::downgraded_resent_bcc;
            case field::downgraded_resent_cc:
                return http::field::downgraded_resent_cc;
            case field::downgraded_resent_from:
                return http::field::downgraded_resent_from;
            case field::downgraded_resent_reply_to:
                return http::field::downgraded_resent_reply_to;
            case field::downgraded_resent_sender:
                return http::field::downgraded_resent_sender;
            case field::downgraded_resent_to:
                return http::field::downgraded_resent_to;
            case field::downgraded_return_path:
                return http::field::downgraded_return_path;
            case field::downgraded_sender:
                return http::field::downgraded_sender;
            case field::downgraded_to:
                return http::field::downgraded_to;
            case field::ediint_features:
                return http::field::ediint_features;
            case field::eesst_version:
                return http::field::eesst_version;
            case field::encoding:
                return http::field::encoding;
            case field::encrypted:
                return http::field::encrypted;
            case field::errors_to:
                return http::field::errors_to;
            case field::etag:
                return http::field::etag;
            case field::expect:
                return http::field::expect;
            case field::expires:
                return http::field::expires;
            case field::expiry_date:
                return http::field::expiry_date;
            case field::ext:
                return http::field::ext;
            case field::followup_to:
                return http::field::followup_to;
            case field::forwarded:
                return http::field::forwarded;
            case field::from:
                return http::field::from;
            case field::generate_delivery_report:
                return http::field::generate_delivery_report;
            case field::getprofile:
                return http::field::getprofile;
            case field::hobareg:
                return http::field::hobareg;
            case field::host:
                return http::field::host;
            case field::http2_settings:
                return http::field::http2_settings;
            case field::if_:
                return http::field::if_;
            case field::if_match:
                return http::field::if_match;
            case field::if_modified_since:
                return http::field::if_modified_since;
            case field::if_none_match:
                return http::field::if_none_match;
            case field::if_range:
                return http::field::if_range;
            case field::if_schedule_tag_match:
                return http::field::if_schedule_tag_match;
            case field::if_unmodified_since:
                return http::field::if_unmodified_since;
            case field::im:
                return http::field::im;
            case field::importance:
                return http::field::importance;
            case field::in_reply_to:
                return http::field::in_reply_to;
            case field::incomplete_copy:
                return http::field::incomplete_copy;
            case field::injection_date:
                return http::field::injection_date;
            case field::injection_info:
                return http::field::injection_info;
            case field::jabber_id:
                return http::field::jabber_id;
            case field::keep_alive:
                return http::field::keep_alive;
            case field::keywords:
                return http::field::keywords;
            case field::label:
                return http::field::label;
            case field::language:
                return http::field::language;
            case field::last_modified:
                return http::field::last_modified;
            case field::latest_delivery_time:
                return http::field::latest_delivery_time;
            case field::lines:
                return http::field::lines;
            case field::link:
                return http::field::link;
            case field::list_archive:
                return http::field::list_archive;
            case field::list_help:
                return http::field::list_help;
            case field::list_id:
                return http::field::list_id;
            case field::list_owner:
                return http::field::list_owner;
            case field::list_post:
                return http::field::list_post;
            case field::list_subscribe:
                return http::field::list_subscribe;
            case field::list_unsubscribe:
                return http::field::list_unsubscribe;
            case field::list_unsubscribe_post:
                return http::field::list_unsubscribe_post;
            case field::location:
                return http::field::location;
            case field::lock_token:
                return http::field::lock_token;
            case field::man:
                return http::field::man;
            case field::max_forwards:
                return http::field::max_forwards;
            case field::memento_datetime:
                return http::field::memento_datetime;
            case field::message_context:
                return http::field::message_context;
            case field::message_id:
                return http::field::message_id;
            case field::message_type:
                return http::field::message_type;
            case field::meter:
                return http::field::meter;
            case field::method_check:
                return http::field::method_check;
            case field::method_check_expires:
                return http::field::method_check_expires;
            case field::mime_version:
                return http::field::mime_version;
            case field::mmhs_acp127_message_identifier:
                return http::field::mmhs_acp127_message_identifier;
            case field::mmhs_authorizing_users:
                return http::field::mmhs_authorizing_users;
            case field::mmhs_codress_message_indicator:
                return http::field::mmhs_codress_message_indicator;
            case field::mmhs_copy_precedence:
                return http::field::mmhs_copy_precedence;
            case field::mmhs_exempted_address:
                return http::field::mmhs_exempted_address;
            case field::mmhs_extended_authorisation_info:
                return http::field::mmhs_extended_authorisation_info;
            case field::mmhs_handling_instructions:
                return http::field::mmhs_handling_instructions;
            case field::mmhs_message_instructions:
                return http::field::mmhs_message_instructions;
            case field::mmhs_message_type:
                return http::field::mmhs_message_type;
            case field::mmhs_originator_plad:
                return http::field::mmhs_originator_plad;
            case field::mmhs_originator_reference:
                return http::field::mmhs_originator_reference;
            case field::mmhs_other_recipients_indicator_cc:
                return http::field::mmhs_other_recipients_indicator_cc;
            case field::mmhs_other_recipients_indicator_to:
                return http::field::mmhs_other_recipients_indicator_to;
            case field::mmhs_primary_precedence:
                return http::field::mmhs_primary_precedence;
            case field::mmhs_subject_indicator_codes:
                return http::field::mmhs_subject_indicator_codes;
            case field::mt_priority:
                return http::field::mt_priority;
            case field::negotiate:
                return http::field::negotiate;
            case field::newsgroups:
                return http::field::newsgroups;
            case field::nntp_posting_date:
                return http::field::nntp_posting_date;
            case field::nntp_posting_host:
                return http::field::nntp_posting_host;
            case field::non_compliance:
                return http::field::non_compliance;
            case field::obsoletes:
                return http::field::obsoletes;
            case field::opt:
                return http::field::opt;
            case field::optional:
                return http::field::optional;
            case field::optional_www_authenticate:
                return http::field::optional_www_authenticate;
            case field::ordering_type:
                return http::field::ordering_type;
            case field::organization:
                return http::field::organization;
            case field::origin:
                return http::field::origin;
            case field::original_encoded_information_types:
                return http::field::original_encoded_information_types;
            case field::original_from:
                return http::field::original_from;
            case field::original_message_id:
                return http::field::original_message_id;
            case field::original_recipient:
                return http::field::original_recipient;
            case field::original_sender:
                return http::field::original_sender;
            case field::original_subject:
                return http::field::original_subject;
            case field::originator_return_address:
                return http::field::originator_return_address;
            case field::overwrite:
                return http::field::overwrite;
            case field::p3p:
                return http::field::p3p;
            case field::path:
                return http::field::path;
            case field::pep:
                return http::field::pep;
            case field::pep_info:
                return http::field::pep_info;
            case field::pics_label:
                return http::field::pics_label;
            case field::position:
                return http::field::position;
            case field::posting_version:
                return http::field::posting_version;
            case field::pragma:
                return http::field::pragma;
            case field::prefer:
                return http::field::prefer;
            case field::preference_applied:
                return http::field::preference_applied;
            case field::prevent_nondelivery_report:
                return http::field::prevent_nondelivery_report;
            case field::priority:
                return http::field::priority;
            case field::privicon:
                return http::field::privicon;
            case field::profileobject:
                return http::field::profileobject;
            case field::protocol:
                return http::field::protocol;
            case field::protocol_info:
                return http::field::protocol_info;
            case field::protocol_query:
                return http::field::protocol_query;
            case field::protocol_request:
                return http::field::protocol_request;
            case field::proxy_authenticate:
                return http::field::proxy_authenticate;
            case field::proxy_authentication_info:
                return http::field::proxy_authentication_info;
            case field::proxy_authorization:
                return http::field::proxy_authorization;
            case field::proxy_connection:
                return http::field::proxy_connection;
            case field::proxy_features:
                return http::field::proxy_features;
            case field::proxy_instruction:
                return http::field::proxy_instruction;
            case field::public_:
                return http::field::public_;
            case field::public_key_pins:
                return http::field::public_key_pins;
            case field::public_key_pins_report_only:
                return http::field::public_key_pins_report_only;
            case field::range:
                return http::field::range;
            case field::received:
                return http::field::received;
            case field::received_spf:
                return http::field::received_spf;
            case field::redirect_ref:
                return http::field::redirect_ref;
            case field::references:
                return http::field::references;
            case field::referer:
                return http::field::referer;
            case field::referer_root:
                return http::field::referer_root;
            case field::relay_version:
                return http::field::relay_version;
            case field::reply_by:
                return http::field::reply_by;
            case field::reply_to:
                return http::field::reply_to;
            case field::require_recipient_valid_since:
                return http::field::require_recipient_valid_since;
            case field::resent_bcc:
                return http::field::resent_bcc;
            case field::resent_cc:
                return http::field::resent_cc;
            case field::resent_date:
                return http::field::resent_date;
            case field::resent_from:
                return http::field::resent_from;
            case field::resent_message_id:
                return http::field::resent_message_id;
            case field::resent_reply_to:
                return http::field::resent_reply_to;
            case field::resent_sender:
                return http::field::resent_sender;
            case field::resent_to:
                return http::field::resent_to;
            case field::resolution_hint:
                return http::field::resolution_hint;
            case field::resolver_location:
                return http::field::resolver_location;
            case field::retry_after:
                return http::field::retry_after;
            case field::return_path:
                return http::field::return_path;
            case field::safe:
                return http::field::safe;
            case field::schedule_reply:
                return http::field::schedule_reply;
            case field::schedule_tag:
                return http::field::schedule_tag;
            case field::sec_fetch_dest:
                return http::field::sec_fetch_dest;
            case field::sec_fetch_mode:
                return http::field::sec_fetch_mode;
            case field::sec_fetch_site:
                return http::field::sec_fetch_site;
            case field::sec_fetch_user:
                return http::field::sec_fetch_user;
            case field::sec_websocket_accept:
                return http::field::sec_websocket_accept;
            case field::sec_websocket_extensions:
                return http::field::sec_websocket_extensions;
            case field::sec_websocket_key:
                return http::field::sec_websocket_key;
            case field::sec_websocket_protocol:
                return http::field::sec_websocket_protocol;
            case field::sec_websocket_version:
                return http::field::sec_websocket_version;
            case field::security_scheme:
                return http::field::security_scheme;
            case field::see_also:
                return http::field::see_also;
            case field::sender:
                return http::field::sender;
            case field::sensitivity:
                return http::field::sensitivity;
            case field::server:
                return http::field::server;
            case field::set_cookie:
                return http::field::set_cookie;
            case field::set_cookie2:
                return http::field::set_cookie2;
            case field::setprofile:
                return http::field::setprofile;
            case field::sio_label:
                return http::field::sio_label;
            case field::sio_label_history:
                return http::field::sio_label_history;
            case field::slug:
                return http::field::slug;
            case field::soapaction:
                return http::field::soapaction;
            case field::solicitation:
                return http::field::solicitation;
            case field::status_uri:
                return http::field::status_uri;
            case field::strict_transport_security:
                return http::field::strict_transport_security;
            case field::subject:
                return http::field::subject;
            case field::subok:
                return http::field::subok;
            case field::subst:
                return http::field::subst;
            case field::summary:
                return http::field::summary;
            case field::supersedes:
                return http::field::supersedes;
            case field::surrogate_capability:
                return http::field::surrogate_capability;
            case field::surrogate_control:
                return http::field::surrogate_control;
            case field::tcn:
                return http::field::tcn;
            case field::te:
                return http::field::te;
            case field::timeout:
                return http::field::timeout;
            case field::title:
                return http::field::title;
            case field::to:
                return http::field::to;
            case field::topic:
                return http::field::topic;
            case field::trailer:
                return http::field::trailer;
            case field::transfer_encoding:
                return http::field::transfer_encoding;
            case field::ttl:
                return http::field::ttl;
            case field::ua_color:
                return http::field::ua_color;
            case field::ua_media:
                return http::field::ua_media;
            case field::ua_pixels:
                return http::field::ua_pixels;
            case field::ua_resolution:
                return http::field::ua_resolution;
            case field::ua_windowpixels:
                return http::field::ua_windowpixels;
            case field::upgrade:
                return http::field::upgrade;
            case field::urgency:
                return http::field::urgency;
            case field::uri:
                return http::field::uri;
            case field::user_agent:
                return http::field::user_agent;
            case field::variant_vary:
                return http::field::variant_vary;
            case field::vary:
                return http::field::vary;
            case field::vbr_info:
                return http::field::vbr_info;
            case field::version:
                return http::field::version;
            case field::via:
                return http::field::via;
            case field::want_digest:
                return http::field::want_digest;
            case field::warning:
                return http::field::warning;
            case field::www_authenticate:
                return http::field::www_authenticate;
            case field::x_archived_at:
                return http::field::x_archived_at;
            case field::x_device_accept:
                return http::field::x_device_accept;
            case field::x_device_accept_charset:
                return http::field::x_device_accept_charset;
            case field::x_device_accept_encoding:
                return http::field::x_device_accept_encoding;
            case field::x_device_accept_language:
                return http::field::x_device_accept_language;
            case field::x_device_user_agent:
                return http::field::x_device_user_agent;
            case field::x_frame_options:
                return http::field::x_frame_options;
            case field::x_mittente:
                return http::field::x_mittente;
            case field::x_pgp_sig:
                return http::field::x_pgp_sig;
            case field::x_ricevuta:
                return http::field::x_ricevuta;
            case field::x_riferimento_message_id:
                return http::field::x_riferimento_message_id;
            case field::x_tiporicevuta:
                return http::field::x_tiporicevuta;
            case field::x_trasporto:
                return http::field::x_trasporto;
            case field::x_verificasicurezza:
                return http::field::x_verificasicurezza;
            case field::x400_content_identifier:
                return http::field::x400_content_identifier;
            case field::x400_content_return:
                return http::field::x400_content_return;
            case field::x400_content_type:
                return http::field::x400_content_type;
            case field::x400_mts_identifier:
                return http::field::x400_mts_identifier;
            case field::x400_originator:
                return http::field::x400_originator;
            case field::x400_received:
                return http::field::x400_received;
            case field::x400_recipients:
                return http::field::x400_recipients;
            case field::x400_trace:
                return http::field::x400_trace;
            case field::xref:
                return http::field::xref;
        }

        // 枚举外的数值（如 static_cast<field>(数字) 构造的未知码）退回 unknown，
        return http::field::unknown;
    }

    /// http::field -> field
    constexpr field
    to_field(http::field v) noexcept
    {
        switch (v)
        {
            case http::field::unknown:
                return field::unknown;
            case http::field::a_im:
                return field::a_im;
            case http::field::accept:
                return field::accept;
            case http::field::accept_additions:
                return field::accept_additions;
            case http::field::accept_charset:
                return field::accept_charset;
            case http::field::accept_datetime:
                return field::accept_datetime;
            case http::field::accept_encoding:
                return field::accept_encoding;
            case http::field::accept_features:
                return field::accept_features;
            case http::field::accept_language:
                return field::accept_language;
            case http::field::accept_patch:
                return field::accept_patch;
            case http::field::accept_post:
                return field::accept_post;
            case http::field::accept_ranges:
                return field::accept_ranges;
            case http::field::access_control:
                return field::access_control;
            case http::field::access_control_allow_credentials:
                return field::access_control_allow_credentials;
            case http::field::access_control_allow_headers:
                return field::access_control_allow_headers;
            case http::field::access_control_allow_methods:
                return field::access_control_allow_methods;
            case http::field::access_control_allow_origin:
                return field::access_control_allow_origin;
            case http::field::access_control_expose_headers:
                return field::access_control_expose_headers;
            case http::field::access_control_max_age:
                return field::access_control_max_age;
            case http::field::access_control_request_headers:
                return field::access_control_request_headers;
            case http::field::access_control_request_method:
                return field::access_control_request_method;
            case http::field::age:
                return field::age;
            case http::field::allow:
                return field::allow;
            case http::field::alpn:
                return field::alpn;
            case http::field::also_control:
                return field::also_control;
            case http::field::alt_svc:
                return field::alt_svc;
            case http::field::alt_used:
                return field::alt_used;
            case http::field::alternate_recipient:
                return field::alternate_recipient;
            case http::field::alternates:
                return field::alternates;
            case http::field::apparently_to:
                return field::apparently_to;
            case http::field::apply_to_redirect_ref:
                return field::apply_to_redirect_ref;
            case http::field::approved:
                return field::approved;
            case http::field::archive:
                return field::archive;
            case http::field::archived_at:
                return field::archived_at;
            case http::field::article_names:
                return field::article_names;
            case http::field::article_updates:
                return field::article_updates;
            case http::field::authentication_control:
                return field::authentication_control;
            case http::field::authentication_info:
                return field::authentication_info;
            case http::field::authentication_results:
                return field::authentication_results;
            case http::field::authorization:
                return field::authorization;
            case http::field::auto_submitted:
                return field::auto_submitted;
            case http::field::autoforwarded:
                return field::autoforwarded;
            case http::field::autosubmitted:
                return field::autosubmitted;
            case http::field::base:
                return field::base;
            case http::field::bcc:
                return field::bcc;
            case http::field::body:
                return field::body;
            case http::field::c_ext:
                return field::c_ext;
            case http::field::c_man:
                return field::c_man;
            case http::field::c_opt:
                return field::c_opt;
            case http::field::c_pep:
                return field::c_pep;
            case http::field::c_pep_info:
                return field::c_pep_info;
            case http::field::cache_control:
                return field::cache_control;
            case http::field::caldav_timezones:
                return field::caldav_timezones;
            case http::field::cancel_key:
                return field::cancel_key;
            case http::field::cancel_lock:
                return field::cancel_lock;
            case http::field::cc:
                return field::cc;
            case http::field::close:
                return field::close;
            case http::field::comments:
                return field::comments;
            case http::field::compliance:
                return field::compliance;
            case http::field::connection:
                return field::connection;
            case http::field::content_alternative:
                return field::content_alternative;
            case http::field::content_base:
                return field::content_base;
            case http::field::content_description:
                return field::content_description;
            case http::field::content_disposition:
                return field::content_disposition;
            case http::field::content_duration:
                return field::content_duration;
            case http::field::content_encoding:
                return field::content_encoding;
            case http::field::content_features:
                return field::content_features;
            case http::field::content_id:
                return field::content_id;
            case http::field::content_identifier:
                return field::content_identifier;
            case http::field::content_language:
                return field::content_language;
            case http::field::content_length:
                return field::content_length;
            case http::field::content_location:
                return field::content_location;
            case http::field::content_md5:
                return field::content_md5;
            case http::field::content_range:
                return field::content_range;
            case http::field::content_return:
                return field::content_return;
            case http::field::content_script_type:
                return field::content_script_type;
            case http::field::content_style_type:
                return field::content_style_type;
            case http::field::content_transfer_encoding:
                return field::content_transfer_encoding;
            case http::field::content_type:
                return field::content_type;
            case http::field::content_version:
                return field::content_version;
            case http::field::control:
                return field::control;
            case http::field::conversion:
                return field::conversion;
            case http::field::conversion_with_loss:
                return field::conversion_with_loss;
            case http::field::cookie:
                return field::cookie;
            case http::field::cookie2:
                return field::cookie2;
            case http::field::cost:
                return field::cost;
            case http::field::dasl:
                return field::dasl;
            case http::field::date:
                return field::date;
            case http::field::date_received:
                return field::date_received;
            case http::field::dav:
                return field::dav;
            case http::field::default_style:
                return field::default_style;
            case http::field::deferred_delivery:
                return field::deferred_delivery;
            case http::field::delivery_date:
                return field::delivery_date;
            case http::field::delta_base:
                return field::delta_base;
            case http::field::depth:
                return field::depth;
            case http::field::derived_from:
                return field::derived_from;
            case http::field::destination:
                return field::destination;
            case http::field::differential_id:
                return field::differential_id;
            case http::field::digest:
                return field::digest;
            case http::field::discarded_x400_ipms_extensions:
                return field::discarded_x400_ipms_extensions;
            case http::field::discarded_x400_mts_extensions:
                return field::discarded_x400_mts_extensions;
            case http::field::disclose_recipients:
                return field::disclose_recipients;
            case http::field::disposition_notification_options:
                return field::disposition_notification_options;
            case http::field::disposition_notification_to:
                return field::disposition_notification_to;
            case http::field::distribution:
                return field::distribution;
            case http::field::dkim_signature:
                return field::dkim_signature;
            case http::field::dl_expansion_history:
                return field::dl_expansion_history;
            case http::field::downgraded_bcc:
                return field::downgraded_bcc;
            case http::field::downgraded_cc:
                return field::downgraded_cc;
            case http::field::downgraded_disposition_notification_to:
                return field::downgraded_disposition_notification_to;
            case http::field::downgraded_final_recipient:
                return field::downgraded_final_recipient;
            case http::field::downgraded_from:
                return field::downgraded_from;
            case http::field::downgraded_in_reply_to:
                return field::downgraded_in_reply_to;
            case http::field::downgraded_mail_from:
                return field::downgraded_mail_from;
            case http::field::downgraded_message_id:
                return field::downgraded_message_id;
            case http::field::downgraded_original_recipient:
                return field::downgraded_original_recipient;
            case http::field::downgraded_rcpt_to:
                return field::downgraded_rcpt_to;
            case http::field::downgraded_references:
                return field::downgraded_references;
            case http::field::downgraded_reply_to:
                return field::downgraded_reply_to;
            case http::field::downgraded_resent_bcc:
                return field::downgraded_resent_bcc;
            case http::field::downgraded_resent_cc:
                return field::downgraded_resent_cc;
            case http::field::downgraded_resent_from:
                return field::downgraded_resent_from;
            case http::field::downgraded_resent_reply_to:
                return field::downgraded_resent_reply_to;
            case http::field::downgraded_resent_sender:
                return field::downgraded_resent_sender;
            case http::field::downgraded_resent_to:
                return field::downgraded_resent_to;
            case http::field::downgraded_return_path:
                return field::downgraded_return_path;
            case http::field::downgraded_sender:
                return field::downgraded_sender;
            case http::field::downgraded_to:
                return field::downgraded_to;
            case http::field::ediint_features:
                return field::ediint_features;
            case http::field::eesst_version:
                return field::eesst_version;
            case http::field::encoding:
                return field::encoding;
            case http::field::encrypted:
                return field::encrypted;
            case http::field::errors_to:
                return field::errors_to;
            case http::field::etag:
                return field::etag;
            case http::field::expect:
                return field::expect;
            case http::field::expires:
                return field::expires;
            case http::field::expiry_date:
                return field::expiry_date;
            case http::field::ext:
                return field::ext;
            case http::field::followup_to:
                return field::followup_to;
            case http::field::forwarded:
                return field::forwarded;
            case http::field::from:
                return field::from;
            case http::field::generate_delivery_report:
                return field::generate_delivery_report;
            case http::field::getprofile:
                return field::getprofile;
            case http::field::hobareg:
                return field::hobareg;
            case http::field::host:
                return field::host;
            case http::field::http2_settings:
                return field::http2_settings;
            case http::field::if_:
                return field::if_;
            case http::field::if_match:
                return field::if_match;
            case http::field::if_modified_since:
                return field::if_modified_since;
            case http::field::if_none_match:
                return field::if_none_match;
            case http::field::if_range:
                return field::if_range;
            case http::field::if_schedule_tag_match:
                return field::if_schedule_tag_match;
            case http::field::if_unmodified_since:
                return field::if_unmodified_since;
            case http::field::im:
                return field::im;
            case http::field::importance:
                return field::importance;
            case http::field::in_reply_to:
                return field::in_reply_to;
            case http::field::incomplete_copy:
                return field::incomplete_copy;
            case http::field::injection_date:
                return field::injection_date;
            case http::field::injection_info:
                return field::injection_info;
            case http::field::jabber_id:
                return field::jabber_id;
            case http::field::keep_alive:
                return field::keep_alive;
            case http::field::keywords:
                return field::keywords;
            case http::field::label:
                return field::label;
            case http::field::language:
                return field::language;
            case http::field::last_modified:
                return field::last_modified;
            case http::field::latest_delivery_time:
                return field::latest_delivery_time;
            case http::field::lines:
                return field::lines;
            case http::field::link:
                return field::link;
            case http::field::list_archive:
                return field::list_archive;
            case http::field::list_help:
                return field::list_help;
            case http::field::list_id:
                return field::list_id;
            case http::field::list_owner:
                return field::list_owner;
            case http::field::list_post:
                return field::list_post;
            case http::field::list_subscribe:
                return field::list_subscribe;
            case http::field::list_unsubscribe:
                return field::list_unsubscribe;
            case http::field::list_unsubscribe_post:
                return field::list_unsubscribe_post;
            case http::field::location:
                return field::location;
            case http::field::lock_token:
                return field::lock_token;
            case http::field::man:
                return field::man;
            case http::field::max_forwards:
                return field::max_forwards;
            case http::field::memento_datetime:
                return field::memento_datetime;
            case http::field::message_context:
                return field::message_context;
            case http::field::message_id:
                return field::message_id;
            case http::field::message_type:
                return field::message_type;
            case http::field::meter:
                return field::meter;
            case http::field::method_check:
                return field::method_check;
            case http::field::method_check_expires:
                return field::method_check_expires;
            case http::field::mime_version:
                return field::mime_version;
            case http::field::mmhs_acp127_message_identifier:
                return field::mmhs_acp127_message_identifier;
            case http::field::mmhs_authorizing_users:
                return field::mmhs_authorizing_users;
            case http::field::mmhs_codress_message_indicator:
                return field::mmhs_codress_message_indicator;
            case http::field::mmhs_copy_precedence:
                return field::mmhs_copy_precedence;
            case http::field::mmhs_exempted_address:
                return field::mmhs_exempted_address;
            case http::field::mmhs_extended_authorisation_info:
                return field::mmhs_extended_authorisation_info;
            case http::field::mmhs_handling_instructions:
                return field::mmhs_handling_instructions;
            case http::field::mmhs_message_instructions:
                return field::mmhs_message_instructions;
            case http::field::mmhs_message_type:
                return field::mmhs_message_type;
            case http::field::mmhs_originator_plad:
                return field::mmhs_originator_plad;
            case http::field::mmhs_originator_reference:
                return field::mmhs_originator_reference;
            case http::field::mmhs_other_recipients_indicator_cc:
                return field::mmhs_other_recipients_indicator_cc;
            case http::field::mmhs_other_recipients_indicator_to:
                return field::mmhs_other_recipients_indicator_to;
            case http::field::mmhs_primary_precedence:
                return field::mmhs_primary_precedence;
            case http::field::mmhs_subject_indicator_codes:
                return field::mmhs_subject_indicator_codes;
            case http::field::mt_priority:
                return field::mt_priority;
            case http::field::negotiate:
                return field::negotiate;
            case http::field::newsgroups:
                return field::newsgroups;
            case http::field::nntp_posting_date:
                return field::nntp_posting_date;
            case http::field::nntp_posting_host:
                return field::nntp_posting_host;
            case http::field::non_compliance:
                return field::non_compliance;
            case http::field::obsoletes:
                return field::obsoletes;
            case http::field::opt:
                return field::opt;
            case http::field::optional:
                return field::optional;
            case http::field::optional_www_authenticate:
                return field::optional_www_authenticate;
            case http::field::ordering_type:
                return field::ordering_type;
            case http::field::organization:
                return field::organization;
            case http::field::origin:
                return field::origin;
            case http::field::original_encoded_information_types:
                return field::original_encoded_information_types;
            case http::field::original_from:
                return field::original_from;
            case http::field::original_message_id:
                return field::original_message_id;
            case http::field::original_recipient:
                return field::original_recipient;
            case http::field::original_sender:
                return field::original_sender;
            case http::field::original_subject:
                return field::original_subject;
            case http::field::originator_return_address:
                return field::originator_return_address;
            case http::field::overwrite:
                return field::overwrite;
            case http::field::p3p:
                return field::p3p;
            case http::field::path:
                return field::path;
            case http::field::pep:
                return field::pep;
            case http::field::pep_info:
                return field::pep_info;
            case http::field::pics_label:
                return field::pics_label;
            case http::field::position:
                return field::position;
            case http::field::posting_version:
                return field::posting_version;
            case http::field::pragma:
                return field::pragma;
            case http::field::prefer:
                return field::prefer;
            case http::field::preference_applied:
                return field::preference_applied;
            case http::field::prevent_nondelivery_report:
                return field::prevent_nondelivery_report;
            case http::field::priority:
                return field::priority;
            case http::field::privicon:
                return field::privicon;
            case http::field::profileobject:
                return field::profileobject;
            case http::field::protocol:
                return field::protocol;
            case http::field::protocol_info:
                return field::protocol_info;
            case http::field::protocol_query:
                return field::protocol_query;
            case http::field::protocol_request:
                return field::protocol_request;
            case http::field::proxy_authenticate:
                return field::proxy_authenticate;
            case http::field::proxy_authentication_info:
                return field::proxy_authentication_info;
            case http::field::proxy_authorization:
                return field::proxy_authorization;
            case http::field::proxy_connection:
                return field::proxy_connection;
            case http::field::proxy_features:
                return field::proxy_features;
            case http::field::proxy_instruction:
                return field::proxy_instruction;
            case http::field::public_:
                return field::public_;
            case http::field::public_key_pins:
                return field::public_key_pins;
            case http::field::public_key_pins_report_only:
                return field::public_key_pins_report_only;
            case http::field::range:
                return field::range;
            case http::field::received:
                return field::received;
            case http::field::received_spf:
                return field::received_spf;
            case http::field::redirect_ref:
                return field::redirect_ref;
            case http::field::references:
                return field::references;
            case http::field::referer:
                return field::referer;
            case http::field::referer_root:
                return field::referer_root;
            case http::field::relay_version:
                return field::relay_version;
            case http::field::reply_by:
                return field::reply_by;
            case http::field::reply_to:
                return field::reply_to;
            case http::field::require_recipient_valid_since:
                return field::require_recipient_valid_since;
            case http::field::resent_bcc:
                return field::resent_bcc;
            case http::field::resent_cc:
                return field::resent_cc;
            case http::field::resent_date:
                return field::resent_date;
            case http::field::resent_from:
                return field::resent_from;
            case http::field::resent_message_id:
                return field::resent_message_id;
            case http::field::resent_reply_to:
                return field::resent_reply_to;
            case http::field::resent_sender:
                return field::resent_sender;
            case http::field::resent_to:
                return field::resent_to;
            case http::field::resolution_hint:
                return field::resolution_hint;
            case http::field::resolver_location:
                return field::resolver_location;
            case http::field::retry_after:
                return field::retry_after;
            case http::field::return_path:
                return field::return_path;
            case http::field::safe:
                return field::safe;
            case http::field::schedule_reply:
                return field::schedule_reply;
            case http::field::schedule_tag:
                return field::schedule_tag;
            case http::field::sec_fetch_dest:
                return field::sec_fetch_dest;
            case http::field::sec_fetch_mode:
                return field::sec_fetch_mode;
            case http::field::sec_fetch_site:
                return field::sec_fetch_site;
            case http::field::sec_fetch_user:
                return field::sec_fetch_user;
            case http::field::sec_websocket_accept:
                return field::sec_websocket_accept;
            case http::field::sec_websocket_extensions:
                return field::sec_websocket_extensions;
            case http::field::sec_websocket_key:
                return field::sec_websocket_key;
            case http::field::sec_websocket_protocol:
                return field::sec_websocket_protocol;
            case http::field::sec_websocket_version:
                return field::sec_websocket_version;
            case http::field::security_scheme:
                return field::security_scheme;
            case http::field::see_also:
                return field::see_also;
            case http::field::sender:
                return field::sender;
            case http::field::sensitivity:
                return field::sensitivity;
            case http::field::server:
                return field::server;
            case http::field::set_cookie:
                return field::set_cookie;
            case http::field::set_cookie2:
                return field::set_cookie2;
            case http::field::setprofile:
                return field::setprofile;
            case http::field::sio_label:
                return field::sio_label;
            case http::field::sio_label_history:
                return field::sio_label_history;
            case http::field::slug:
                return field::slug;
            case http::field::soapaction:
                return field::soapaction;
            case http::field::solicitation:
                return field::solicitation;
            case http::field::status_uri:
                return field::status_uri;
            case http::field::strict_transport_security:
                return field::strict_transport_security;
            case http::field::subject:
                return field::subject;
            case http::field::subok:
                return field::subok;
            case http::field::subst:
                return field::subst;
            case http::field::summary:
                return field::summary;
            case http::field::supersedes:
                return field::supersedes;
            case http::field::surrogate_capability:
                return field::surrogate_capability;
            case http::field::surrogate_control:
                return field::surrogate_control;
            case http::field::tcn:
                return field::tcn;
            case http::field::te:
                return field::te;
            case http::field::timeout:
                return field::timeout;
            case http::field::title:
                return field::title;
            case http::field::to:
                return field::to;
            case http::field::topic:
                return field::topic;
            case http::field::trailer:
                return field::trailer;
            case http::field::transfer_encoding:
                return field::transfer_encoding;
            case http::field::ttl:
                return field::ttl;
            case http::field::ua_color:
                return field::ua_color;
            case http::field::ua_media:
                return field::ua_media;
            case http::field::ua_pixels:
                return field::ua_pixels;
            case http::field::ua_resolution:
                return field::ua_resolution;
            case http::field::ua_windowpixels:
                return field::ua_windowpixels;
            case http::field::upgrade:
                return field::upgrade;
            case http::field::urgency:
                return field::urgency;
            case http::field::uri:
                return field::uri;
            case http::field::user_agent:
                return field::user_agent;
            case http::field::variant_vary:
                return field::variant_vary;
            case http::field::vary:
                return field::vary;
            case http::field::vbr_info:
                return field::vbr_info;
            case http::field::version:
                return field::version;
            case http::field::via:
                return field::via;
            case http::field::want_digest:
                return field::want_digest;
            case http::field::warning:
                return field::warning;
            case http::field::www_authenticate:
                return field::www_authenticate;
            case http::field::x_archived_at:
                return field::x_archived_at;
            case http::field::x_device_accept:
                return field::x_device_accept;
            case http::field::x_device_accept_charset:
                return field::x_device_accept_charset;
            case http::field::x_device_accept_encoding:
                return field::x_device_accept_encoding;
            case http::field::x_device_accept_language:
                return field::x_device_accept_language;
            case http::field::x_device_user_agent:
                return field::x_device_user_agent;
            case http::field::x_frame_options:
                return field::x_frame_options;
            case http::field::x_mittente:
                return field::x_mittente;
            case http::field::x_pgp_sig:
                return field::x_pgp_sig;
            case http::field::x_ricevuta:
                return field::x_ricevuta;
            case http::field::x_riferimento_message_id:
                return field::x_riferimento_message_id;
            case http::field::x_tiporicevuta:
                return field::x_tiporicevuta;
            case http::field::x_trasporto:
                return field::x_trasporto;
            case http::field::x_verificasicurezza:
                return field::x_verificasicurezza;
            case http::field::x400_content_identifier:
                return field::x400_content_identifier;
            case http::field::x400_content_return:
                return field::x400_content_return;
            case http::field::x400_content_type:
                return field::x400_content_type;
            case http::field::x400_mts_identifier:
                return field::x400_mts_identifier;
            case http::field::x400_originator:
                return field::x400_originator;
            case http::field::x400_received:
                return field::x400_received;
            case http::field::x400_recipients:
                return field::x400_recipients;
            case http::field::x400_trace:
                return field::x400_trace;
            case http::field::xref:
                return field::xref;
        }

        // 枚举外的数值（如 static_cast<http::field>(数字) 构造的未知码）退回 unknown，
        return field::unknown;
    }

    // ---- 互转换逐值校验 ----
    //
    // 覆盖不全的 switch 会走到末尾兜底 return（返回 unknown），所以这批断言同时锁住
    // 「每个枚举值都接到了正确的 beast 值」——漏一个 case 就是编译失败，且不依赖
    // -Wswitch（MSVC 的 C4061/C4062 要 /W4，本项目没开）。

    // method -> http::verb
    static_assert(to_verb(method::unknown) == http::verb::unknown, "enum_conv: method::unknown 映射缺失或错位");
    static_assert(to_verb(method::delete_) == http::verb::delete_, "enum_conv: method::delete_ 映射缺失或错位");
    static_assert(to_verb(method::get) == http::verb::get, "enum_conv: method::get 映射缺失或错位");
    static_assert(to_verb(method::head) == http::verb::head, "enum_conv: method::head 映射缺失或错位");
    static_assert(to_verb(method::post) == http::verb::post, "enum_conv: method::post 映射缺失或错位");
    static_assert(to_verb(method::put) == http::verb::put, "enum_conv: method::put 映射缺失或错位");
    static_assert(to_verb(method::connect) == http::verb::connect, "enum_conv: method::connect 映射缺失或错位");
    static_assert(to_verb(method::options) == http::verb::options, "enum_conv: method::options 映射缺失或错位");
    static_assert(to_verb(method::trace) == http::verb::trace, "enum_conv: method::trace 映射缺失或错位");
    static_assert(to_verb(method::copy) == http::verb::copy, "enum_conv: method::copy 映射缺失或错位");
    static_assert(to_verb(method::lock) == http::verb::lock, "enum_conv: method::lock 映射缺失或错位");
    static_assert(to_verb(method::mkcol) == http::verb::mkcol, "enum_conv: method::mkcol 映射缺失或错位");
    static_assert(to_verb(method::move) == http::verb::move, "enum_conv: method::move 映射缺失或错位");
    static_assert(to_verb(method::propfind) == http::verb::propfind, "enum_conv: method::propfind 映射缺失或错位");
    static_assert(to_verb(method::proppatch) == http::verb::proppatch, "enum_conv: method::proppatch 映射缺失或错位");
    static_assert(to_verb(method::search) == http::verb::search, "enum_conv: method::search 映射缺失或错位");
    static_assert(to_verb(method::unlock) == http::verb::unlock, "enum_conv: method::unlock 映射缺失或错位");
    static_assert(to_verb(method::bind) == http::verb::bind, "enum_conv: method::bind 映射缺失或错位");
    static_assert(to_verb(method::rebind) == http::verb::rebind, "enum_conv: method::rebind 映射缺失或错位");
    static_assert(to_verb(method::unbind) == http::verb::unbind, "enum_conv: method::unbind 映射缺失或错位");
    static_assert(to_verb(method::acl) == http::verb::acl, "enum_conv: method::acl 映射缺失或错位");
    static_assert(to_verb(method::report) == http::verb::report, "enum_conv: method::report 映射缺失或错位");
    static_assert(to_verb(method::mkactivity) == http::verb::mkactivity,
                  "enum_conv: method::mkactivity 映射缺失或错位");
    static_assert(to_verb(method::checkout) == http::verb::checkout, "enum_conv: method::checkout 映射缺失或错位");
    static_assert(to_verb(method::merge) == http::verb::merge, "enum_conv: method::merge 映射缺失或错位");
    static_assert(to_verb(method::msearch) == http::verb::msearch, "enum_conv: method::msearch 映射缺失或错位");
    static_assert(to_verb(method::notify) == http::verb::notify, "enum_conv: method::notify 映射缺失或错位");
    static_assert(to_verb(method::subscribe) == http::verb::subscribe, "enum_conv: method::subscribe 映射缺失或错位");
    static_assert(to_verb(method::unsubscribe) == http::verb::unsubscribe,
                  "enum_conv: method::unsubscribe 映射缺失或错位");
    static_assert(to_verb(method::patch) == http::verb::patch, "enum_conv: method::patch 映射缺失或错位");
    static_assert(to_verb(method::purge) == http::verb::purge, "enum_conv: method::purge 映射缺失或错位");
    static_assert(to_verb(method::mkcalendar) == http::verb::mkcalendar,
                  "enum_conv: method::mkcalendar 映射缺失或错位");
    static_assert(to_verb(method::link) == http::verb::link, "enum_conv: method::link 映射缺失或错位");
    static_assert(to_verb(method::unlink) == http::verb::unlink, "enum_conv: method::unlink 映射缺失或错位");

    // http::verb -> method
    static_assert(to_method(http::verb::unknown) == method::unknown, "enum_conv: http::verb::unknown 映射缺失或错位");
    static_assert(to_method(http::verb::delete_) == method::delete_, "enum_conv: http::verb::delete_ 映射缺失或错位");
    static_assert(to_method(http::verb::get) == method::get, "enum_conv: http::verb::get 映射缺失或错位");
    static_assert(to_method(http::verb::head) == method::head, "enum_conv: http::verb::head 映射缺失或错位");
    static_assert(to_method(http::verb::post) == method::post, "enum_conv: http::verb::post 映射缺失或错位");
    static_assert(to_method(http::verb::put) == method::put, "enum_conv: http::verb::put 映射缺失或错位");
    static_assert(to_method(http::verb::connect) == method::connect, "enum_conv: http::verb::connect 映射缺失或错位");
    static_assert(to_method(http::verb::options) == method::options, "enum_conv: http::verb::options 映射缺失或错位");
    static_assert(to_method(http::verb::trace) == method::trace, "enum_conv: http::verb::trace 映射缺失或错位");
    static_assert(to_method(http::verb::copy) == method::copy, "enum_conv: http::verb::copy 映射缺失或错位");
    static_assert(to_method(http::verb::lock) == method::lock, "enum_conv: http::verb::lock 映射缺失或错位");
    static_assert(to_method(http::verb::mkcol) == method::mkcol, "enum_conv: http::verb::mkcol 映射缺失或错位");
    static_assert(to_method(http::verb::move) == method::move, "enum_conv: http::verb::move 映射缺失或错位");
    static_assert(to_method(http::verb::propfind) == method::propfind,
                  "enum_conv: http::verb::propfind 映射缺失或错位");
    static_assert(to_method(http::verb::proppatch) == method::proppatch,
                  "enum_conv: http::verb::proppatch 映射缺失或错位");
    static_assert(to_method(http::verb::search) == method::search, "enum_conv: http::verb::search 映射缺失或错位");
    static_assert(to_method(http::verb::unlock) == method::unlock, "enum_conv: http::verb::unlock 映射缺失或错位");
    static_assert(to_method(http::verb::bind) == method::bind, "enum_conv: http::verb::bind 映射缺失或错位");
    static_assert(to_method(http::verb::rebind) == method::rebind, "enum_conv: http::verb::rebind 映射缺失或错位");
    static_assert(to_method(http::verb::unbind) == method::unbind, "enum_conv: http::verb::unbind 映射缺失或错位");
    static_assert(to_method(http::verb::acl) == method::acl, "enum_conv: http::verb::acl 映射缺失或错位");
    static_assert(to_method(http::verb::report) == method::report, "enum_conv: http::verb::report 映射缺失或错位");
    static_assert(to_method(http::verb::mkactivity) == method::mkactivity,
                  "enum_conv: http::verb::mkactivity 映射缺失或错位");
    static_assert(to_method(http::verb::checkout) == method::checkout,
                  "enum_conv: http::verb::checkout 映射缺失或错位");
    static_assert(to_method(http::verb::merge) == method::merge, "enum_conv: http::verb::merge 映射缺失或错位");
    static_assert(to_method(http::verb::msearch) == method::msearch, "enum_conv: http::verb::msearch 映射缺失或错位");
    static_assert(to_method(http::verb::notify) == method::notify, "enum_conv: http::verb::notify 映射缺失或错位");
    static_assert(to_method(http::verb::subscribe) == method::subscribe,
                  "enum_conv: http::verb::subscribe 映射缺失或错位");
    static_assert(to_method(http::verb::unsubscribe) == method::unsubscribe,
                  "enum_conv: http::verb::unsubscribe 映射缺失或错位");
    static_assert(to_method(http::verb::patch) == method::patch, "enum_conv: http::verb::patch 映射缺失或错位");
    static_assert(to_method(http::verb::purge) == method::purge, "enum_conv: http::verb::purge 映射缺失或错位");
    static_assert(to_method(http::verb::mkcalendar) == method::mkcalendar,
                  "enum_conv: http::verb::mkcalendar 映射缺失或错位");
    static_assert(to_method(http::verb::link) == method::link, "enum_conv: http::verb::link 映射缺失或错位");
    static_assert(to_method(http::verb::unlink) == method::unlink, "enum_conv: http::verb::unlink 映射缺失或错位");

    // status -> http::status
    static_assert(to_status(status::unknown) == http::status::unknown, "enum_conv: status::unknown 映射缺失或错位");
    static_assert(to_status(status::continue_) == http::status::continue_,
                  "enum_conv: status::continue_ 映射缺失或错位");
    static_assert(to_status(status::switching_protocols) == http::status::switching_protocols,
                  "enum_conv: status::switching_protocols 映射缺失或错位");
    static_assert(to_status(status::processing) == http::status::processing,
                  "enum_conv: status::processing 映射缺失或错位");
    static_assert(to_status(status::early_hints) == http::status::early_hints,
                  "enum_conv: status::early_hints 映射缺失或错位");
    static_assert(to_status(status::ok) == http::status::ok, "enum_conv: status::ok 映射缺失或错位");
    static_assert(to_status(status::created) == http::status::created, "enum_conv: status::created 映射缺失或错位");
    static_assert(to_status(status::accepted) == http::status::accepted, "enum_conv: status::accepted 映射缺失或错位");
    static_assert(to_status(status::non_authoritative_information) == http::status::non_authoritative_information,
                  "enum_conv: status::non_authoritative_information 映射缺失或错位");
    static_assert(to_status(status::no_content) == http::status::no_content,
                  "enum_conv: status::no_content 映射缺失或错位");
    static_assert(to_status(status::reset_content) == http::status::reset_content,
                  "enum_conv: status::reset_content 映射缺失或错位");
    static_assert(to_status(status::partial_content) == http::status::partial_content,
                  "enum_conv: status::partial_content 映射缺失或错位");
    static_assert(to_status(status::multi_status) == http::status::multi_status,
                  "enum_conv: status::multi_status 映射缺失或错位");
    static_assert(to_status(status::already_reported) == http::status::already_reported,
                  "enum_conv: status::already_reported 映射缺失或错位");
    static_assert(to_status(status::im_used) == http::status::im_used, "enum_conv: status::im_used 映射缺失或错位");
    static_assert(to_status(status::multiple_choices) == http::status::multiple_choices,
                  "enum_conv: status::multiple_choices 映射缺失或错位");
    static_assert(to_status(status::moved_permanently) == http::status::moved_permanently,
                  "enum_conv: status::moved_permanently 映射缺失或错位");
    static_assert(to_status(status::found) == http::status::found, "enum_conv: status::found 映射缺失或错位");
    static_assert(to_status(status::see_other) == http::status::see_other,
                  "enum_conv: status::see_other 映射缺失或错位");
    static_assert(to_status(status::not_modified) == http::status::not_modified,
                  "enum_conv: status::not_modified 映射缺失或错位");
    static_assert(to_status(status::use_proxy) == http::status::use_proxy,
                  "enum_conv: status::use_proxy 映射缺失或错位");
    static_assert(to_status(status::temporary_redirect) == http::status::temporary_redirect,
                  "enum_conv: status::temporary_redirect 映射缺失或错位");
    static_assert(to_status(status::permanent_redirect) == http::status::permanent_redirect,
                  "enum_conv: status::permanent_redirect 映射缺失或错位");
    static_assert(to_status(status::bad_request) == http::status::bad_request,
                  "enum_conv: status::bad_request 映射缺失或错位");
    static_assert(to_status(status::unauthorized) == http::status::unauthorized,
                  "enum_conv: status::unauthorized 映射缺失或错位");
    static_assert(to_status(status::payment_required) == http::status::payment_required,
                  "enum_conv: status::payment_required 映射缺失或错位");
    static_assert(to_status(status::forbidden) == http::status::forbidden,
                  "enum_conv: status::forbidden 映射缺失或错位");
    static_assert(to_status(status::not_found) == http::status::not_found,
                  "enum_conv: status::not_found 映射缺失或错位");
    static_assert(to_status(status::method_not_allowed) == http::status::method_not_allowed,
                  "enum_conv: status::method_not_allowed 映射缺失或错位");
    static_assert(to_status(status::not_acceptable) == http::status::not_acceptable,
                  "enum_conv: status::not_acceptable 映射缺失或错位");
    static_assert(to_status(status::proxy_authentication_required) == http::status::proxy_authentication_required,
                  "enum_conv: status::proxy_authentication_required 映射缺失或错位");
    static_assert(to_status(status::request_timeout) == http::status::request_timeout,
                  "enum_conv: status::request_timeout 映射缺失或错位");
    static_assert(to_status(status::conflict) == http::status::conflict, "enum_conv: status::conflict 映射缺失或错位");
    static_assert(to_status(status::gone) == http::status::gone, "enum_conv: status::gone 映射缺失或错位");
    static_assert(to_status(status::length_required) == http::status::length_required,
                  "enum_conv: status::length_required 映射缺失或错位");
    static_assert(to_status(status::precondition_failed) == http::status::precondition_failed,
                  "enum_conv: status::precondition_failed 映射缺失或错位");
    static_assert(to_status(status::payload_too_large) == http::status::payload_too_large,
                  "enum_conv: status::payload_too_large 映射缺失或错位");
    static_assert(to_status(status::uri_too_long) == http::status::uri_too_long,
                  "enum_conv: status::uri_too_long 映射缺失或错位");
    static_assert(to_status(status::unsupported_media_type) == http::status::unsupported_media_type,
                  "enum_conv: status::unsupported_media_type 映射缺失或错位");
    static_assert(to_status(status::range_not_satisfiable) == http::status::range_not_satisfiable,
                  "enum_conv: status::range_not_satisfiable 映射缺失或错位");
    static_assert(to_status(status::expectation_failed) == http::status::expectation_failed,
                  "enum_conv: status::expectation_failed 映射缺失或错位");
    static_assert(to_status(status::i_am_a_teapot) == http::status::i_am_a_teapot,
                  "enum_conv: status::i_am_a_teapot 映射缺失或错位");
    static_assert(to_status(status::misdirected_request) == http::status::misdirected_request,
                  "enum_conv: status::misdirected_request 映射缺失或错位");
    static_assert(to_status(status::unprocessable_entity) == http::status::unprocessable_entity,
                  "enum_conv: status::unprocessable_entity 映射缺失或错位");
    static_assert(to_status(status::locked) == http::status::locked, "enum_conv: status::locked 映射缺失或错位");
    static_assert(to_status(status::failed_dependency) == http::status::failed_dependency,
                  "enum_conv: status::failed_dependency 映射缺失或错位");
    static_assert(to_status(status::too_early) == http::status::too_early,
                  "enum_conv: status::too_early 映射缺失或错位");
    static_assert(to_status(status::upgrade_required) == http::status::upgrade_required,
                  "enum_conv: status::upgrade_required 映射缺失或错位");
    static_assert(to_status(status::precondition_required) == http::status::precondition_required,
                  "enum_conv: status::precondition_required 映射缺失或错位");
    static_assert(to_status(status::too_many_requests) == http::status::too_many_requests,
                  "enum_conv: status::too_many_requests 映射缺失或错位");
    static_assert(to_status(status::request_header_fields_too_large) == http::status::request_header_fields_too_large,
                  "enum_conv: status::request_header_fields_too_large 映射缺失或错位");
    static_assert(to_status(status::unavailable_for_legal_reasons) == http::status::unavailable_for_legal_reasons,
                  "enum_conv: status::unavailable_for_legal_reasons 映射缺失或错位");
    static_assert(to_status(status::internal_server_error) == http::status::internal_server_error,
                  "enum_conv: status::internal_server_error 映射缺失或错位");
    static_assert(to_status(status::not_implemented) == http::status::not_implemented,
                  "enum_conv: status::not_implemented 映射缺失或错位");
    static_assert(to_status(status::bad_gateway) == http::status::bad_gateway,
                  "enum_conv: status::bad_gateway 映射缺失或错位");
    static_assert(to_status(status::service_unavailable) == http::status::service_unavailable,
                  "enum_conv: status::service_unavailable 映射缺失或错位");
    static_assert(to_status(status::gateway_timeout) == http::status::gateway_timeout,
                  "enum_conv: status::gateway_timeout 映射缺失或错位");
    static_assert(to_status(status::http_version_not_supported) == http::status::http_version_not_supported,
                  "enum_conv: status::http_version_not_supported 映射缺失或错位");
    static_assert(to_status(status::variant_also_negotiates) == http::status::variant_also_negotiates,
                  "enum_conv: status::variant_also_negotiates 映射缺失或错位");
    static_assert(to_status(status::insufficient_storage) == http::status::insufficient_storage,
                  "enum_conv: status::insufficient_storage 映射缺失或错位");
    static_assert(to_status(status::loop_detected) == http::status::loop_detected,
                  "enum_conv: status::loop_detected 映射缺失或错位");
    static_assert(to_status(status::not_extended) == http::status::not_extended,
                  "enum_conv: status::not_extended 映射缺失或错位");
    static_assert(to_status(status::network_authentication_required) == http::status::network_authentication_required,
                  "enum_conv: status::network_authentication_required 映射缺失或错位");

    // http::status -> status
    static_assert(to_status(http::status::unknown) == status::unknown,
                  "enum_conv: http::status::unknown 映射缺失或错位");
    static_assert(to_status(http::status::continue_) == status::continue_,
                  "enum_conv: http::status::continue_ 映射缺失或错位");
    static_assert(to_status(http::status::switching_protocols) == status::switching_protocols,
                  "enum_conv: http::status::switching_protocols 映射缺失或错位");
    static_assert(to_status(http::status::processing) == status::processing,
                  "enum_conv: http::status::processing 映射缺失或错位");
    static_assert(to_status(http::status::early_hints) == status::early_hints,
                  "enum_conv: http::status::early_hints 映射缺失或错位");
    static_assert(to_status(http::status::ok) == status::ok, "enum_conv: http::status::ok 映射缺失或错位");
    static_assert(to_status(http::status::created) == status::created,
                  "enum_conv: http::status::created 映射缺失或错位");
    static_assert(to_status(http::status::accepted) == status::accepted,
                  "enum_conv: http::status::accepted 映射缺失或错位");
    static_assert(to_status(http::status::non_authoritative_information) == status::non_authoritative_information,
                  "enum_conv: http::status::non_authoritative_information 映射缺失或错位");
    static_assert(to_status(http::status::no_content) == status::no_content,
                  "enum_conv: http::status::no_content 映射缺失或错位");
    static_assert(to_status(http::status::reset_content) == status::reset_content,
                  "enum_conv: http::status::reset_content 映射缺失或错位");
    static_assert(to_status(http::status::partial_content) == status::partial_content,
                  "enum_conv: http::status::partial_content 映射缺失或错位");
    static_assert(to_status(http::status::multi_status) == status::multi_status,
                  "enum_conv: http::status::multi_status 映射缺失或错位");
    static_assert(to_status(http::status::already_reported) == status::already_reported,
                  "enum_conv: http::status::already_reported 映射缺失或错位");
    static_assert(to_status(http::status::im_used) == status::im_used,
                  "enum_conv: http::status::im_used 映射缺失或错位");
    static_assert(to_status(http::status::multiple_choices) == status::multiple_choices,
                  "enum_conv: http::status::multiple_choices 映射缺失或错位");
    static_assert(to_status(http::status::moved_permanently) == status::moved_permanently,
                  "enum_conv: http::status::moved_permanently 映射缺失或错位");
    static_assert(to_status(http::status::found) == status::found, "enum_conv: http::status::found 映射缺失或错位");
    static_assert(to_status(http::status::see_other) == status::see_other,
                  "enum_conv: http::status::see_other 映射缺失或错位");
    static_assert(to_status(http::status::not_modified) == status::not_modified,
                  "enum_conv: http::status::not_modified 映射缺失或错位");
    static_assert(to_status(http::status::use_proxy) == status::use_proxy,
                  "enum_conv: http::status::use_proxy 映射缺失或错位");
    static_assert(to_status(http::status::temporary_redirect) == status::temporary_redirect,
                  "enum_conv: http::status::temporary_redirect 映射缺失或错位");
    static_assert(to_status(http::status::permanent_redirect) == status::permanent_redirect,
                  "enum_conv: http::status::permanent_redirect 映射缺失或错位");
    static_assert(to_status(http::status::bad_request) == status::bad_request,
                  "enum_conv: http::status::bad_request 映射缺失或错位");
    static_assert(to_status(http::status::unauthorized) == status::unauthorized,
                  "enum_conv: http::status::unauthorized 映射缺失或错位");
    static_assert(to_status(http::status::payment_required) == status::payment_required,
                  "enum_conv: http::status::payment_required 映射缺失或错位");
    static_assert(to_status(http::status::forbidden) == status::forbidden,
                  "enum_conv: http::status::forbidden 映射缺失或错位");
    static_assert(to_status(http::status::not_found) == status::not_found,
                  "enum_conv: http::status::not_found 映射缺失或错位");
    static_assert(to_status(http::status::method_not_allowed) == status::method_not_allowed,
                  "enum_conv: http::status::method_not_allowed 映射缺失或错位");
    static_assert(to_status(http::status::not_acceptable) == status::not_acceptable,
                  "enum_conv: http::status::not_acceptable 映射缺失或错位");
    static_assert(to_status(http::status::proxy_authentication_required) == status::proxy_authentication_required,
                  "enum_conv: http::status::proxy_authentication_required 映射缺失或错位");
    static_assert(to_status(http::status::request_timeout) == status::request_timeout,
                  "enum_conv: http::status::request_timeout 映射缺失或错位");
    static_assert(to_status(http::status::conflict) == status::conflict,
                  "enum_conv: http::status::conflict 映射缺失或错位");
    static_assert(to_status(http::status::gone) == status::gone, "enum_conv: http::status::gone 映射缺失或错位");
    static_assert(to_status(http::status::length_required) == status::length_required,
                  "enum_conv: http::status::length_required 映射缺失或错位");
    static_assert(to_status(http::status::precondition_failed) == status::precondition_failed,
                  "enum_conv: http::status::precondition_failed 映射缺失或错位");
    static_assert(to_status(http::status::payload_too_large) == status::payload_too_large,
                  "enum_conv: http::status::payload_too_large 映射缺失或错位");
    static_assert(to_status(http::status::uri_too_long) == status::uri_too_long,
                  "enum_conv: http::status::uri_too_long 映射缺失或错位");
    static_assert(to_status(http::status::unsupported_media_type) == status::unsupported_media_type,
                  "enum_conv: http::status::unsupported_media_type 映射缺失或错位");
    static_assert(to_status(http::status::range_not_satisfiable) == status::range_not_satisfiable,
                  "enum_conv: http::status::range_not_satisfiable 映射缺失或错位");
    static_assert(to_status(http::status::expectation_failed) == status::expectation_failed,
                  "enum_conv: http::status::expectation_failed 映射缺失或错位");
    static_assert(to_status(http::status::i_am_a_teapot) == status::i_am_a_teapot,
                  "enum_conv: http::status::i_am_a_teapot 映射缺失或错位");
    static_assert(to_status(http::status::misdirected_request) == status::misdirected_request,
                  "enum_conv: http::status::misdirected_request 映射缺失或错位");
    static_assert(to_status(http::status::unprocessable_entity) == status::unprocessable_entity,
                  "enum_conv: http::status::unprocessable_entity 映射缺失或错位");
    static_assert(to_status(http::status::locked) == status::locked, "enum_conv: http::status::locked 映射缺失或错位");
    static_assert(to_status(http::status::failed_dependency) == status::failed_dependency,
                  "enum_conv: http::status::failed_dependency 映射缺失或错位");
    static_assert(to_status(http::status::too_early) == status::too_early,
                  "enum_conv: http::status::too_early 映射缺失或错位");
    static_assert(to_status(http::status::upgrade_required) == status::upgrade_required,
                  "enum_conv: http::status::upgrade_required 映射缺失或错位");
    static_assert(to_status(http::status::precondition_required) == status::precondition_required,
                  "enum_conv: http::status::precondition_required 映射缺失或错位");
    static_assert(to_status(http::status::too_many_requests) == status::too_many_requests,
                  "enum_conv: http::status::too_many_requests 映射缺失或错位");
    static_assert(to_status(http::status::request_header_fields_too_large) == status::request_header_fields_too_large,
                  "enum_conv: http::status::request_header_fields_too_large 映射缺失或错位");
    static_assert(to_status(http::status::unavailable_for_legal_reasons) == status::unavailable_for_legal_reasons,
                  "enum_conv: http::status::unavailable_for_legal_reasons 映射缺失或错位");
    static_assert(to_status(http::status::internal_server_error) == status::internal_server_error,
                  "enum_conv: http::status::internal_server_error 映射缺失或错位");
    static_assert(to_status(http::status::not_implemented) == status::not_implemented,
                  "enum_conv: http::status::not_implemented 映射缺失或错位");
    static_assert(to_status(http::status::bad_gateway) == status::bad_gateway,
                  "enum_conv: http::status::bad_gateway 映射缺失或错位");
    static_assert(to_status(http::status::service_unavailable) == status::service_unavailable,
                  "enum_conv: http::status::service_unavailable 映射缺失或错位");
    static_assert(to_status(http::status::gateway_timeout) == status::gateway_timeout,
                  "enum_conv: http::status::gateway_timeout 映射缺失或错位");
    static_assert(to_status(http::status::http_version_not_supported) == status::http_version_not_supported,
                  "enum_conv: http::status::http_version_not_supported 映射缺失或错位");
    static_assert(to_status(http::status::variant_also_negotiates) == status::variant_also_negotiates,
                  "enum_conv: http::status::variant_also_negotiates 映射缺失或错位");
    static_assert(to_status(http::status::insufficient_storage) == status::insufficient_storage,
                  "enum_conv: http::status::insufficient_storage 映射缺失或错位");
    static_assert(to_status(http::status::loop_detected) == status::loop_detected,
                  "enum_conv: http::status::loop_detected 映射缺失或错位");
    static_assert(to_status(http::status::not_extended) == status::not_extended,
                  "enum_conv: http::status::not_extended 映射缺失或错位");
    static_assert(to_status(http::status::network_authentication_required) == status::network_authentication_required,
                  "enum_conv: http::status::network_authentication_required 映射缺失或错位");

    // field -> http::field
    static_assert(to_field(field::unknown) == http::field::unknown, "enum_conv: field::unknown 映射缺失或错位");
    static_assert(to_field(field::a_im) == http::field::a_im, "enum_conv: field::a_im 映射缺失或错位");
    static_assert(to_field(field::accept) == http::field::accept, "enum_conv: field::accept 映射缺失或错位");
    static_assert(to_field(field::accept_additions) == http::field::accept_additions,
                  "enum_conv: field::accept_additions 映射缺失或错位");
    static_assert(to_field(field::accept_charset) == http::field::accept_charset,
                  "enum_conv: field::accept_charset 映射缺失或错位");
    static_assert(to_field(field::accept_datetime) == http::field::accept_datetime,
                  "enum_conv: field::accept_datetime 映射缺失或错位");
    static_assert(to_field(field::accept_encoding) == http::field::accept_encoding,
                  "enum_conv: field::accept_encoding 映射缺失或错位");
    static_assert(to_field(field::accept_features) == http::field::accept_features,
                  "enum_conv: field::accept_features 映射缺失或错位");
    static_assert(to_field(field::accept_language) == http::field::accept_language,
                  "enum_conv: field::accept_language 映射缺失或错位");
    static_assert(to_field(field::accept_patch) == http::field::accept_patch,
                  "enum_conv: field::accept_patch 映射缺失或错位");
    static_assert(to_field(field::accept_post) == http::field::accept_post,
                  "enum_conv: field::accept_post 映射缺失或错位");
    static_assert(to_field(field::accept_ranges) == http::field::accept_ranges,
                  "enum_conv: field::accept_ranges 映射缺失或错位");
    static_assert(to_field(field::access_control) == http::field::access_control,
                  "enum_conv: field::access_control 映射缺失或错位");
    static_assert(to_field(field::access_control_allow_credentials) == http::field::access_control_allow_credentials,
                  "enum_conv: field::access_control_allow_credentials 映射缺失或错位");
    static_assert(to_field(field::access_control_allow_headers) == http::field::access_control_allow_headers,
                  "enum_conv: field::access_control_allow_headers 映射缺失或错位");
    static_assert(to_field(field::access_control_allow_methods) == http::field::access_control_allow_methods,
                  "enum_conv: field::access_control_allow_methods 映射缺失或错位");
    static_assert(to_field(field::access_control_allow_origin) == http::field::access_control_allow_origin,
                  "enum_conv: field::access_control_allow_origin 映射缺失或错位");
    static_assert(to_field(field::access_control_expose_headers) == http::field::access_control_expose_headers,
                  "enum_conv: field::access_control_expose_headers 映射缺失或错位");
    static_assert(to_field(field::access_control_max_age) == http::field::access_control_max_age,
                  "enum_conv: field::access_control_max_age 映射缺失或错位");
    static_assert(to_field(field::access_control_request_headers) == http::field::access_control_request_headers,
                  "enum_conv: field::access_control_request_headers 映射缺失或错位");
    static_assert(to_field(field::access_control_request_method) == http::field::access_control_request_method,
                  "enum_conv: field::access_control_request_method 映射缺失或错位");
    static_assert(to_field(field::age) == http::field::age, "enum_conv: field::age 映射缺失或错位");
    static_assert(to_field(field::allow) == http::field::allow, "enum_conv: field::allow 映射缺失或错位");
    static_assert(to_field(field::alpn) == http::field::alpn, "enum_conv: field::alpn 映射缺失或错位");
    static_assert(to_field(field::also_control) == http::field::also_control,
                  "enum_conv: field::also_control 映射缺失或错位");
    static_assert(to_field(field::alt_svc) == http::field::alt_svc, "enum_conv: field::alt_svc 映射缺失或错位");
    static_assert(to_field(field::alt_used) == http::field::alt_used, "enum_conv: field::alt_used 映射缺失或错位");
    static_assert(to_field(field::alternate_recipient) == http::field::alternate_recipient,
                  "enum_conv: field::alternate_recipient 映射缺失或错位");
    static_assert(to_field(field::alternates) == http::field::alternates,
                  "enum_conv: field::alternates 映射缺失或错位");
    static_assert(to_field(field::apparently_to) == http::field::apparently_to,
                  "enum_conv: field::apparently_to 映射缺失或错位");
    static_assert(to_field(field::apply_to_redirect_ref) == http::field::apply_to_redirect_ref,
                  "enum_conv: field::apply_to_redirect_ref 映射缺失或错位");
    static_assert(to_field(field::approved) == http::field::approved, "enum_conv: field::approved 映射缺失或错位");
    static_assert(to_field(field::archive) == http::field::archive, "enum_conv: field::archive 映射缺失或错位");
    static_assert(to_field(field::archived_at) == http::field::archived_at,
                  "enum_conv: field::archived_at 映射缺失或错位");
    static_assert(to_field(field::article_names) == http::field::article_names,
                  "enum_conv: field::article_names 映射缺失或错位");
    static_assert(to_field(field::article_updates) == http::field::article_updates,
                  "enum_conv: field::article_updates 映射缺失或错位");
    static_assert(to_field(field::authentication_control) == http::field::authentication_control,
                  "enum_conv: field::authentication_control 映射缺失或错位");
    static_assert(to_field(field::authentication_info) == http::field::authentication_info,
                  "enum_conv: field::authentication_info 映射缺失或错位");
    static_assert(to_field(field::authentication_results) == http::field::authentication_results,
                  "enum_conv: field::authentication_results 映射缺失或错位");
    static_assert(to_field(field::authorization) == http::field::authorization,
                  "enum_conv: field::authorization 映射缺失或错位");
    static_assert(to_field(field::auto_submitted) == http::field::auto_submitted,
                  "enum_conv: field::auto_submitted 映射缺失或错位");
    static_assert(to_field(field::autoforwarded) == http::field::autoforwarded,
                  "enum_conv: field::autoforwarded 映射缺失或错位");
    static_assert(to_field(field::autosubmitted) == http::field::autosubmitted,
                  "enum_conv: field::autosubmitted 映射缺失或错位");
    static_assert(to_field(field::base) == http::field::base, "enum_conv: field::base 映射缺失或错位");
    static_assert(to_field(field::bcc) == http::field::bcc, "enum_conv: field::bcc 映射缺失或错位");
    static_assert(to_field(field::body) == http::field::body, "enum_conv: field::body 映射缺失或错位");
    static_assert(to_field(field::c_ext) == http::field::c_ext, "enum_conv: field::c_ext 映射缺失或错位");
    static_assert(to_field(field::c_man) == http::field::c_man, "enum_conv: field::c_man 映射缺失或错位");
    static_assert(to_field(field::c_opt) == http::field::c_opt, "enum_conv: field::c_opt 映射缺失或错位");
    static_assert(to_field(field::c_pep) == http::field::c_pep, "enum_conv: field::c_pep 映射缺失或错位");
    static_assert(to_field(field::c_pep_info) == http::field::c_pep_info,
                  "enum_conv: field::c_pep_info 映射缺失或错位");
    static_assert(to_field(field::cache_control) == http::field::cache_control,
                  "enum_conv: field::cache_control 映射缺失或错位");
    static_assert(to_field(field::caldav_timezones) == http::field::caldav_timezones,
                  "enum_conv: field::caldav_timezones 映射缺失或错位");
    static_assert(to_field(field::cancel_key) == http::field::cancel_key,
                  "enum_conv: field::cancel_key 映射缺失或错位");
    static_assert(to_field(field::cancel_lock) == http::field::cancel_lock,
                  "enum_conv: field::cancel_lock 映射缺失或错位");
    static_assert(to_field(field::cc) == http::field::cc, "enum_conv: field::cc 映射缺失或错位");
    static_assert(to_field(field::close) == http::field::close, "enum_conv: field::close 映射缺失或错位");
    static_assert(to_field(field::comments) == http::field::comments, "enum_conv: field::comments 映射缺失或错位");
    static_assert(to_field(field::compliance) == http::field::compliance,
                  "enum_conv: field::compliance 映射缺失或错位");
    static_assert(to_field(field::connection) == http::field::connection,
                  "enum_conv: field::connection 映射缺失或错位");
    static_assert(to_field(field::content_alternative) == http::field::content_alternative,
                  "enum_conv: field::content_alternative 映射缺失或错位");
    static_assert(to_field(field::content_base) == http::field::content_base,
                  "enum_conv: field::content_base 映射缺失或错位");
    static_assert(to_field(field::content_description) == http::field::content_description,
                  "enum_conv: field::content_description 映射缺失或错位");
    static_assert(to_field(field::content_disposition) == http::field::content_disposition,
                  "enum_conv: field::content_disposition 映射缺失或错位");
    static_assert(to_field(field::content_duration) == http::field::content_duration,
                  "enum_conv: field::content_duration 映射缺失或错位");
    static_assert(to_field(field::content_encoding) == http::field::content_encoding,
                  "enum_conv: field::content_encoding 映射缺失或错位");
    static_assert(to_field(field::content_features) == http::field::content_features,
                  "enum_conv: field::content_features 映射缺失或错位");
    static_assert(to_field(field::content_id) == http::field::content_id,
                  "enum_conv: field::content_id 映射缺失或错位");
    static_assert(to_field(field::content_identifier) == http::field::content_identifier,
                  "enum_conv: field::content_identifier 映射缺失或错位");
    static_assert(to_field(field::content_language) == http::field::content_language,
                  "enum_conv: field::content_language 映射缺失或错位");
    static_assert(to_field(field::content_length) == http::field::content_length,
                  "enum_conv: field::content_length 映射缺失或错位");
    static_assert(to_field(field::content_location) == http::field::content_location,
                  "enum_conv: field::content_location 映射缺失或错位");
    static_assert(to_field(field::content_md5) == http::field::content_md5,
                  "enum_conv: field::content_md5 映射缺失或错位");
    static_assert(to_field(field::content_range) == http::field::content_range,
                  "enum_conv: field::content_range 映射缺失或错位");
    static_assert(to_field(field::content_return) == http::field::content_return,
                  "enum_conv: field::content_return 映射缺失或错位");
    static_assert(to_field(field::content_script_type) == http::field::content_script_type,
                  "enum_conv: field::content_script_type 映射缺失或错位");
    static_assert(to_field(field::content_style_type) == http::field::content_style_type,
                  "enum_conv: field::content_style_type 映射缺失或错位");
    static_assert(to_field(field::content_transfer_encoding) == http::field::content_transfer_encoding,
                  "enum_conv: field::content_transfer_encoding 映射缺失或错位");
    static_assert(to_field(field::content_type) == http::field::content_type,
                  "enum_conv: field::content_type 映射缺失或错位");
    static_assert(to_field(field::content_version) == http::field::content_version,
                  "enum_conv: field::content_version 映射缺失或错位");
    static_assert(to_field(field::control) == http::field::control, "enum_conv: field::control 映射缺失或错位");
    static_assert(to_field(field::conversion) == http::field::conversion,
                  "enum_conv: field::conversion 映射缺失或错位");
    static_assert(to_field(field::conversion_with_loss) == http::field::conversion_with_loss,
                  "enum_conv: field::conversion_with_loss 映射缺失或错位");
    static_assert(to_field(field::cookie) == http::field::cookie, "enum_conv: field::cookie 映射缺失或错位");
    static_assert(to_field(field::cookie2) == http::field::cookie2, "enum_conv: field::cookie2 映射缺失或错位");
    static_assert(to_field(field::cost) == http::field::cost, "enum_conv: field::cost 映射缺失或错位");
    static_assert(to_field(field::dasl) == http::field::dasl, "enum_conv: field::dasl 映射缺失或错位");
    static_assert(to_field(field::date) == http::field::date, "enum_conv: field::date 映射缺失或错位");
    static_assert(to_field(field::date_received) == http::field::date_received,
                  "enum_conv: field::date_received 映射缺失或错位");
    static_assert(to_field(field::dav) == http::field::dav, "enum_conv: field::dav 映射缺失或错位");
    static_assert(to_field(field::default_style) == http::field::default_style,
                  "enum_conv: field::default_style 映射缺失或错位");
    static_assert(to_field(field::deferred_delivery) == http::field::deferred_delivery,
                  "enum_conv: field::deferred_delivery 映射缺失或错位");
    static_assert(to_field(field::delivery_date) == http::field::delivery_date,
                  "enum_conv: field::delivery_date 映射缺失或错位");
    static_assert(to_field(field::delta_base) == http::field::delta_base,
                  "enum_conv: field::delta_base 映射缺失或错位");
    static_assert(to_field(field::depth) == http::field::depth, "enum_conv: field::depth 映射缺失或错位");
    static_assert(to_field(field::derived_from) == http::field::derived_from,
                  "enum_conv: field::derived_from 映射缺失或错位");
    static_assert(to_field(field::destination) == http::field::destination,
                  "enum_conv: field::destination 映射缺失或错位");
    static_assert(to_field(field::differential_id) == http::field::differential_id,
                  "enum_conv: field::differential_id 映射缺失或错位");
    static_assert(to_field(field::digest) == http::field::digest, "enum_conv: field::digest 映射缺失或错位");
    static_assert(to_field(field::discarded_x400_ipms_extensions) == http::field::discarded_x400_ipms_extensions,
                  "enum_conv: field::discarded_x400_ipms_extensions 映射缺失或错位");
    static_assert(to_field(field::discarded_x400_mts_extensions) == http::field::discarded_x400_mts_extensions,
                  "enum_conv: field::discarded_x400_mts_extensions 映射缺失或错位");
    static_assert(to_field(field::disclose_recipients) == http::field::disclose_recipients,
                  "enum_conv: field::disclose_recipients 映射缺失或错位");
    static_assert(to_field(field::disposition_notification_options) == http::field::disposition_notification_options,
                  "enum_conv: field::disposition_notification_options 映射缺失或错位");
    static_assert(to_field(field::disposition_notification_to) == http::field::disposition_notification_to,
                  "enum_conv: field::disposition_notification_to 映射缺失或错位");
    static_assert(to_field(field::distribution) == http::field::distribution,
                  "enum_conv: field::distribution 映射缺失或错位");
    static_assert(to_field(field::dkim_signature) == http::field::dkim_signature,
                  "enum_conv: field::dkim_signature 映射缺失或错位");
    static_assert(to_field(field::dl_expansion_history) == http::field::dl_expansion_history,
                  "enum_conv: field::dl_expansion_history 映射缺失或错位");
    static_assert(to_field(field::downgraded_bcc) == http::field::downgraded_bcc,
                  "enum_conv: field::downgraded_bcc 映射缺失或错位");
    static_assert(to_field(field::downgraded_cc) == http::field::downgraded_cc,
                  "enum_conv: field::downgraded_cc 映射缺失或错位");
    static_assert(to_field(field::downgraded_disposition_notification_to)
                      == http::field::downgraded_disposition_notification_to,
                  "enum_conv: field::downgraded_disposition_notification_to 映射缺失或错位");
    static_assert(to_field(field::downgraded_final_recipient) == http::field::downgraded_final_recipient,
                  "enum_conv: field::downgraded_final_recipient 映射缺失或错位");
    static_assert(to_field(field::downgraded_from) == http::field::downgraded_from,
                  "enum_conv: field::downgraded_from 映射缺失或错位");
    static_assert(to_field(field::downgraded_in_reply_to) == http::field::downgraded_in_reply_to,
                  "enum_conv: field::downgraded_in_reply_to 映射缺失或错位");
    static_assert(to_field(field::downgraded_mail_from) == http::field::downgraded_mail_from,
                  "enum_conv: field::downgraded_mail_from 映射缺失或错位");
    static_assert(to_field(field::downgraded_message_id) == http::field::downgraded_message_id,
                  "enum_conv: field::downgraded_message_id 映射缺失或错位");
    static_assert(to_field(field::downgraded_original_recipient) == http::field::downgraded_original_recipient,
                  "enum_conv: field::downgraded_original_recipient 映射缺失或错位");
    static_assert(to_field(field::downgraded_rcpt_to) == http::field::downgraded_rcpt_to,
                  "enum_conv: field::downgraded_rcpt_to 映射缺失或错位");
    static_assert(to_field(field::downgraded_references) == http::field::downgraded_references,
                  "enum_conv: field::downgraded_references 映射缺失或错位");
    static_assert(to_field(field::downgraded_reply_to) == http::field::downgraded_reply_to,
                  "enum_conv: field::downgraded_reply_to 映射缺失或错位");
    static_assert(to_field(field::downgraded_resent_bcc) == http::field::downgraded_resent_bcc,
                  "enum_conv: field::downgraded_resent_bcc 映射缺失或错位");
    static_assert(to_field(field::downgraded_resent_cc) == http::field::downgraded_resent_cc,
                  "enum_conv: field::downgraded_resent_cc 映射缺失或错位");
    static_assert(to_field(field::downgraded_resent_from) == http::field::downgraded_resent_from,
                  "enum_conv: field::downgraded_resent_from 映射缺失或错位");
    static_assert(to_field(field::downgraded_resent_reply_to) == http::field::downgraded_resent_reply_to,
                  "enum_conv: field::downgraded_resent_reply_to 映射缺失或错位");
    static_assert(to_field(field::downgraded_resent_sender) == http::field::downgraded_resent_sender,
                  "enum_conv: field::downgraded_resent_sender 映射缺失或错位");
    static_assert(to_field(field::downgraded_resent_to) == http::field::downgraded_resent_to,
                  "enum_conv: field::downgraded_resent_to 映射缺失或错位");
    static_assert(to_field(field::downgraded_return_path) == http::field::downgraded_return_path,
                  "enum_conv: field::downgraded_return_path 映射缺失或错位");
    static_assert(to_field(field::downgraded_sender) == http::field::downgraded_sender,
                  "enum_conv: field::downgraded_sender 映射缺失或错位");
    static_assert(to_field(field::downgraded_to) == http::field::downgraded_to,
                  "enum_conv: field::downgraded_to 映射缺失或错位");
    static_assert(to_field(field::ediint_features) == http::field::ediint_features,
                  "enum_conv: field::ediint_features 映射缺失或错位");
    static_assert(to_field(field::eesst_version) == http::field::eesst_version,
                  "enum_conv: field::eesst_version 映射缺失或错位");
    static_assert(to_field(field::encoding) == http::field::encoding, "enum_conv: field::encoding 映射缺失或错位");
    static_assert(to_field(field::encrypted) == http::field::encrypted, "enum_conv: field::encrypted 映射缺失或错位");
    static_assert(to_field(field::errors_to) == http::field::errors_to, "enum_conv: field::errors_to 映射缺失或错位");
    static_assert(to_field(field::etag) == http::field::etag, "enum_conv: field::etag 映射缺失或错位");
    static_assert(to_field(field::expect) == http::field::expect, "enum_conv: field::expect 映射缺失或错位");
    static_assert(to_field(field::expires) == http::field::expires, "enum_conv: field::expires 映射缺失或错位");
    static_assert(to_field(field::expiry_date) == http::field::expiry_date,
                  "enum_conv: field::expiry_date 映射缺失或错位");
    static_assert(to_field(field::ext) == http::field::ext, "enum_conv: field::ext 映射缺失或错位");
    static_assert(to_field(field::followup_to) == http::field::followup_to,
                  "enum_conv: field::followup_to 映射缺失或错位");
    static_assert(to_field(field::forwarded) == http::field::forwarded, "enum_conv: field::forwarded 映射缺失或错位");
    static_assert(to_field(field::from) == http::field::from, "enum_conv: field::from 映射缺失或错位");
    static_assert(to_field(field::generate_delivery_report) == http::field::generate_delivery_report,
                  "enum_conv: field::generate_delivery_report 映射缺失或错位");
    static_assert(to_field(field::getprofile) == http::field::getprofile,
                  "enum_conv: field::getprofile 映射缺失或错位");
    static_assert(to_field(field::hobareg) == http::field::hobareg, "enum_conv: field::hobareg 映射缺失或错位");
    static_assert(to_field(field::host) == http::field::host, "enum_conv: field::host 映射缺失或错位");
    static_assert(to_field(field::http2_settings) == http::field::http2_settings,
                  "enum_conv: field::http2_settings 映射缺失或错位");
    static_assert(to_field(field::if_) == http::field::if_, "enum_conv: field::if_ 映射缺失或错位");
    static_assert(to_field(field::if_match) == http::field::if_match, "enum_conv: field::if_match 映射缺失或错位");
    static_assert(to_field(field::if_modified_since) == http::field::if_modified_since,
                  "enum_conv: field::if_modified_since 映射缺失或错位");
    static_assert(to_field(field::if_none_match) == http::field::if_none_match,
                  "enum_conv: field::if_none_match 映射缺失或错位");
    static_assert(to_field(field::if_range) == http::field::if_range, "enum_conv: field::if_range 映射缺失或错位");
    static_assert(to_field(field::if_schedule_tag_match) == http::field::if_schedule_tag_match,
                  "enum_conv: field::if_schedule_tag_match 映射缺失或错位");
    static_assert(to_field(field::if_unmodified_since) == http::field::if_unmodified_since,
                  "enum_conv: field::if_unmodified_since 映射缺失或错位");
    static_assert(to_field(field::im) == http::field::im, "enum_conv: field::im 映射缺失或错位");
    static_assert(to_field(field::importance) == http::field::importance,
                  "enum_conv: field::importance 映射缺失或错位");
    static_assert(to_field(field::in_reply_to) == http::field::in_reply_to,
                  "enum_conv: field::in_reply_to 映射缺失或错位");
    static_assert(to_field(field::incomplete_copy) == http::field::incomplete_copy,
                  "enum_conv: field::incomplete_copy 映射缺失或错位");
    static_assert(to_field(field::injection_date) == http::field::injection_date,
                  "enum_conv: field::injection_date 映射缺失或错位");
    static_assert(to_field(field::injection_info) == http::field::injection_info,
                  "enum_conv: field::injection_info 映射缺失或错位");
    static_assert(to_field(field::jabber_id) == http::field::jabber_id, "enum_conv: field::jabber_id 映射缺失或错位");
    static_assert(to_field(field::keep_alive) == http::field::keep_alive,
                  "enum_conv: field::keep_alive 映射缺失或错位");
    static_assert(to_field(field::keywords) == http::field::keywords, "enum_conv: field::keywords 映射缺失或错位");
    static_assert(to_field(field::label) == http::field::label, "enum_conv: field::label 映射缺失或错位");
    static_assert(to_field(field::language) == http::field::language, "enum_conv: field::language 映射缺失或错位");
    static_assert(to_field(field::last_modified) == http::field::last_modified,
                  "enum_conv: field::last_modified 映射缺失或错位");
    static_assert(to_field(field::latest_delivery_time) == http::field::latest_delivery_time,
                  "enum_conv: field::latest_delivery_time 映射缺失或错位");
    static_assert(to_field(field::lines) == http::field::lines, "enum_conv: field::lines 映射缺失或错位");
    static_assert(to_field(field::link) == http::field::link, "enum_conv: field::link 映射缺失或错位");
    static_assert(to_field(field::list_archive) == http::field::list_archive,
                  "enum_conv: field::list_archive 映射缺失或错位");
    static_assert(to_field(field::list_help) == http::field::list_help, "enum_conv: field::list_help 映射缺失或错位");
    static_assert(to_field(field::list_id) == http::field::list_id, "enum_conv: field::list_id 映射缺失或错位");
    static_assert(to_field(field::list_owner) == http::field::list_owner,
                  "enum_conv: field::list_owner 映射缺失或错位");
    static_assert(to_field(field::list_post) == http::field::list_post, "enum_conv: field::list_post 映射缺失或错位");
    static_assert(to_field(field::list_subscribe) == http::field::list_subscribe,
                  "enum_conv: field::list_subscribe 映射缺失或错位");
    static_assert(to_field(field::list_unsubscribe) == http::field::list_unsubscribe,
                  "enum_conv: field::list_unsubscribe 映射缺失或错位");
    static_assert(to_field(field::list_unsubscribe_post) == http::field::list_unsubscribe_post,
                  "enum_conv: field::list_unsubscribe_post 映射缺失或错位");
    static_assert(to_field(field::location) == http::field::location, "enum_conv: field::location 映射缺失或错位");
    static_assert(to_field(field::lock_token) == http::field::lock_token,
                  "enum_conv: field::lock_token 映射缺失或错位");
    static_assert(to_field(field::man) == http::field::man, "enum_conv: field::man 映射缺失或错位");
    static_assert(to_field(field::max_forwards) == http::field::max_forwards,
                  "enum_conv: field::max_forwards 映射缺失或错位");
    static_assert(to_field(field::memento_datetime) == http::field::memento_datetime,
                  "enum_conv: field::memento_datetime 映射缺失或错位");
    static_assert(to_field(field::message_context) == http::field::message_context,
                  "enum_conv: field::message_context 映射缺失或错位");
    static_assert(to_field(field::message_id) == http::field::message_id,
                  "enum_conv: field::message_id 映射缺失或错位");
    static_assert(to_field(field::message_type) == http::field::message_type,
                  "enum_conv: field::message_type 映射缺失或错位");
    static_assert(to_field(field::meter) == http::field::meter, "enum_conv: field::meter 映射缺失或错位");
    static_assert(to_field(field::method_check) == http::field::method_check,
                  "enum_conv: field::method_check 映射缺失或错位");
    static_assert(to_field(field::method_check_expires) == http::field::method_check_expires,
                  "enum_conv: field::method_check_expires 映射缺失或错位");
    static_assert(to_field(field::mime_version) == http::field::mime_version,
                  "enum_conv: field::mime_version 映射缺失或错位");
    static_assert(to_field(field::mmhs_acp127_message_identifier) == http::field::mmhs_acp127_message_identifier,
                  "enum_conv: field::mmhs_acp127_message_identifier 映射缺失或错位");
    static_assert(to_field(field::mmhs_authorizing_users) == http::field::mmhs_authorizing_users,
                  "enum_conv: field::mmhs_authorizing_users 映射缺失或错位");
    static_assert(to_field(field::mmhs_codress_message_indicator) == http::field::mmhs_codress_message_indicator,
                  "enum_conv: field::mmhs_codress_message_indicator 映射缺失或错位");
    static_assert(to_field(field::mmhs_copy_precedence) == http::field::mmhs_copy_precedence,
                  "enum_conv: field::mmhs_copy_precedence 映射缺失或错位");
    static_assert(to_field(field::mmhs_exempted_address) == http::field::mmhs_exempted_address,
                  "enum_conv: field::mmhs_exempted_address 映射缺失或错位");
    static_assert(to_field(field::mmhs_extended_authorisation_info) == http::field::mmhs_extended_authorisation_info,
                  "enum_conv: field::mmhs_extended_authorisation_info 映射缺失或错位");
    static_assert(to_field(field::mmhs_handling_instructions) == http::field::mmhs_handling_instructions,
                  "enum_conv: field::mmhs_handling_instructions 映射缺失或错位");
    static_assert(to_field(field::mmhs_message_instructions) == http::field::mmhs_message_instructions,
                  "enum_conv: field::mmhs_message_instructions 映射缺失或错位");
    static_assert(to_field(field::mmhs_message_type) == http::field::mmhs_message_type,
                  "enum_conv: field::mmhs_message_type 映射缺失或错位");
    static_assert(to_field(field::mmhs_originator_plad) == http::field::mmhs_originator_plad,
                  "enum_conv: field::mmhs_originator_plad 映射缺失或错位");
    static_assert(to_field(field::mmhs_originator_reference) == http::field::mmhs_originator_reference,
                  "enum_conv: field::mmhs_originator_reference 映射缺失或错位");
    static_assert(to_field(field::mmhs_other_recipients_indicator_cc)
                      == http::field::mmhs_other_recipients_indicator_cc,
                  "enum_conv: field::mmhs_other_recipients_indicator_cc 映射缺失或错位");
    static_assert(to_field(field::mmhs_other_recipients_indicator_to)
                      == http::field::mmhs_other_recipients_indicator_to,
                  "enum_conv: field::mmhs_other_recipients_indicator_to 映射缺失或错位");
    static_assert(to_field(field::mmhs_primary_precedence) == http::field::mmhs_primary_precedence,
                  "enum_conv: field::mmhs_primary_precedence 映射缺失或错位");
    static_assert(to_field(field::mmhs_subject_indicator_codes) == http::field::mmhs_subject_indicator_codes,
                  "enum_conv: field::mmhs_subject_indicator_codes 映射缺失或错位");
    static_assert(to_field(field::mt_priority) == http::field::mt_priority,
                  "enum_conv: field::mt_priority 映射缺失或错位");
    static_assert(to_field(field::negotiate) == http::field::negotiate, "enum_conv: field::negotiate 映射缺失或错位");
    static_assert(to_field(field::newsgroups) == http::field::newsgroups,
                  "enum_conv: field::newsgroups 映射缺失或错位");
    static_assert(to_field(field::nntp_posting_date) == http::field::nntp_posting_date,
                  "enum_conv: field::nntp_posting_date 映射缺失或错位");
    static_assert(to_field(field::nntp_posting_host) == http::field::nntp_posting_host,
                  "enum_conv: field::nntp_posting_host 映射缺失或错位");
    static_assert(to_field(field::non_compliance) == http::field::non_compliance,
                  "enum_conv: field::non_compliance 映射缺失或错位");
    static_assert(to_field(field::obsoletes) == http::field::obsoletes, "enum_conv: field::obsoletes 映射缺失或错位");
    static_assert(to_field(field::opt) == http::field::opt, "enum_conv: field::opt 映射缺失或错位");
    static_assert(to_field(field::optional) == http::field::optional, "enum_conv: field::optional 映射缺失或错位");
    static_assert(to_field(field::optional_www_authenticate) == http::field::optional_www_authenticate,
                  "enum_conv: field::optional_www_authenticate 映射缺失或错位");
    static_assert(to_field(field::ordering_type) == http::field::ordering_type,
                  "enum_conv: field::ordering_type 映射缺失或错位");
    static_assert(to_field(field::organization) == http::field::organization,
                  "enum_conv: field::organization 映射缺失或错位");
    static_assert(to_field(field::origin) == http::field::origin, "enum_conv: field::origin 映射缺失或错位");
    static_assert(to_field(field::original_encoded_information_types)
                      == http::field::original_encoded_information_types,
                  "enum_conv: field::original_encoded_information_types 映射缺失或错位");
    static_assert(to_field(field::original_from) == http::field::original_from,
                  "enum_conv: field::original_from 映射缺失或错位");
    static_assert(to_field(field::original_message_id) == http::field::original_message_id,
                  "enum_conv: field::original_message_id 映射缺失或错位");
    static_assert(to_field(field::original_recipient) == http::field::original_recipient,
                  "enum_conv: field::original_recipient 映射缺失或错位");
    static_assert(to_field(field::original_sender) == http::field::original_sender,
                  "enum_conv: field::original_sender 映射缺失或错位");
    static_assert(to_field(field::original_subject) == http::field::original_subject,
                  "enum_conv: field::original_subject 映射缺失或错位");
    static_assert(to_field(field::originator_return_address) == http::field::originator_return_address,
                  "enum_conv: field::originator_return_address 映射缺失或错位");
    static_assert(to_field(field::overwrite) == http::field::overwrite, "enum_conv: field::overwrite 映射缺失或错位");
    static_assert(to_field(field::p3p) == http::field::p3p, "enum_conv: field::p3p 映射缺失或错位");
    static_assert(to_field(field::path) == http::field::path, "enum_conv: field::path 映射缺失或错位");
    static_assert(to_field(field::pep) == http::field::pep, "enum_conv: field::pep 映射缺失或错位");
    static_assert(to_field(field::pep_info) == http::field::pep_info, "enum_conv: field::pep_info 映射缺失或错位");
    static_assert(to_field(field::pics_label) == http::field::pics_label,
                  "enum_conv: field::pics_label 映射缺失或错位");
    static_assert(to_field(field::position) == http::field::position, "enum_conv: field::position 映射缺失或错位");
    static_assert(to_field(field::posting_version) == http::field::posting_version,
                  "enum_conv: field::posting_version 映射缺失或错位");
    static_assert(to_field(field::pragma) == http::field::pragma, "enum_conv: field::pragma 映射缺失或错位");
    static_assert(to_field(field::prefer) == http::field::prefer, "enum_conv: field::prefer 映射缺失或错位");
    static_assert(to_field(field::preference_applied) == http::field::preference_applied,
                  "enum_conv: field::preference_applied 映射缺失或错位");
    static_assert(to_field(field::prevent_nondelivery_report) == http::field::prevent_nondelivery_report,
                  "enum_conv: field::prevent_nondelivery_report 映射缺失或错位");
    static_assert(to_field(field::priority) == http::field::priority, "enum_conv: field::priority 映射缺失或错位");
    static_assert(to_field(field::privicon) == http::field::privicon, "enum_conv: field::privicon 映射缺失或错位");
    static_assert(to_field(field::profileobject) == http::field::profileobject,
                  "enum_conv: field::profileobject 映射缺失或错位");
    static_assert(to_field(field::protocol) == http::field::protocol, "enum_conv: field::protocol 映射缺失或错位");
    static_assert(to_field(field::protocol_info) == http::field::protocol_info,
                  "enum_conv: field::protocol_info 映射缺失或错位");
    static_assert(to_field(field::protocol_query) == http::field::protocol_query,
                  "enum_conv: field::protocol_query 映射缺失或错位");
    static_assert(to_field(field::protocol_request) == http::field::protocol_request,
                  "enum_conv: field::protocol_request 映射缺失或错位");
    static_assert(to_field(field::proxy_authenticate) == http::field::proxy_authenticate,
                  "enum_conv: field::proxy_authenticate 映射缺失或错位");
    static_assert(to_field(field::proxy_authentication_info) == http::field::proxy_authentication_info,
                  "enum_conv: field::proxy_authentication_info 映射缺失或错位");
    static_assert(to_field(field::proxy_authorization) == http::field::proxy_authorization,
                  "enum_conv: field::proxy_authorization 映射缺失或错位");
    static_assert(to_field(field::proxy_connection) == http::field::proxy_connection,
                  "enum_conv: field::proxy_connection 映射缺失或错位");
    static_assert(to_field(field::proxy_features) == http::field::proxy_features,
                  "enum_conv: field::proxy_features 映射缺失或错位");
    static_assert(to_field(field::proxy_instruction) == http::field::proxy_instruction,
                  "enum_conv: field::proxy_instruction 映射缺失或错位");
    static_assert(to_field(field::public_) == http::field::public_, "enum_conv: field::public_ 映射缺失或错位");
    static_assert(to_field(field::public_key_pins) == http::field::public_key_pins,
                  "enum_conv: field::public_key_pins 映射缺失或错位");
    static_assert(to_field(field::public_key_pins_report_only) == http::field::public_key_pins_report_only,
                  "enum_conv: field::public_key_pins_report_only 映射缺失或错位");
    static_assert(to_field(field::range) == http::field::range, "enum_conv: field::range 映射缺失或错位");
    static_assert(to_field(field::received) == http::field::received, "enum_conv: field::received 映射缺失或错位");
    static_assert(to_field(field::received_spf) == http::field::received_spf,
                  "enum_conv: field::received_spf 映射缺失或错位");
    static_assert(to_field(field::redirect_ref) == http::field::redirect_ref,
                  "enum_conv: field::redirect_ref 映射缺失或错位");
    static_assert(to_field(field::references) == http::field::references,
                  "enum_conv: field::references 映射缺失或错位");
    static_assert(to_field(field::referer) == http::field::referer, "enum_conv: field::referer 映射缺失或错位");
    static_assert(to_field(field::referer_root) == http::field::referer_root,
                  "enum_conv: field::referer_root 映射缺失或错位");
    static_assert(to_field(field::relay_version) == http::field::relay_version,
                  "enum_conv: field::relay_version 映射缺失或错位");
    static_assert(to_field(field::reply_by) == http::field::reply_by, "enum_conv: field::reply_by 映射缺失或错位");
    static_assert(to_field(field::reply_to) == http::field::reply_to, "enum_conv: field::reply_to 映射缺失或错位");
    static_assert(to_field(field::require_recipient_valid_since) == http::field::require_recipient_valid_since,
                  "enum_conv: field::require_recipient_valid_since 映射缺失或错位");
    static_assert(to_field(field::resent_bcc) == http::field::resent_bcc,
                  "enum_conv: field::resent_bcc 映射缺失或错位");
    static_assert(to_field(field::resent_cc) == http::field::resent_cc, "enum_conv: field::resent_cc 映射缺失或错位");
    static_assert(to_field(field::resent_date) == http::field::resent_date,
                  "enum_conv: field::resent_date 映射缺失或错位");
    static_assert(to_field(field::resent_from) == http::field::resent_from,
                  "enum_conv: field::resent_from 映射缺失或错位");
    static_assert(to_field(field::resent_message_id) == http::field::resent_message_id,
                  "enum_conv: field::resent_message_id 映射缺失或错位");
    static_assert(to_field(field::resent_reply_to) == http::field::resent_reply_to,
                  "enum_conv: field::resent_reply_to 映射缺失或错位");
    static_assert(to_field(field::resent_sender) == http::field::resent_sender,
                  "enum_conv: field::resent_sender 映射缺失或错位");
    static_assert(to_field(field::resent_to) == http::field::resent_to, "enum_conv: field::resent_to 映射缺失或错位");
    static_assert(to_field(field::resolution_hint) == http::field::resolution_hint,
                  "enum_conv: field::resolution_hint 映射缺失或错位");
    static_assert(to_field(field::resolver_location) == http::field::resolver_location,
                  "enum_conv: field::resolver_location 映射缺失或错位");
    static_assert(to_field(field::retry_after) == http::field::retry_after,
                  "enum_conv: field::retry_after 映射缺失或错位");
    static_assert(to_field(field::return_path) == http::field::return_path,
                  "enum_conv: field::return_path 映射缺失或错位");
    static_assert(to_field(field::safe) == http::field::safe, "enum_conv: field::safe 映射缺失或错位");
    static_assert(to_field(field::schedule_reply) == http::field::schedule_reply,
                  "enum_conv: field::schedule_reply 映射缺失或错位");
    static_assert(to_field(field::schedule_tag) == http::field::schedule_tag,
                  "enum_conv: field::schedule_tag 映射缺失或错位");
    static_assert(to_field(field::sec_fetch_dest) == http::field::sec_fetch_dest,
                  "enum_conv: field::sec_fetch_dest 映射缺失或错位");
    static_assert(to_field(field::sec_fetch_mode) == http::field::sec_fetch_mode,
                  "enum_conv: field::sec_fetch_mode 映射缺失或错位");
    static_assert(to_field(field::sec_fetch_site) == http::field::sec_fetch_site,
                  "enum_conv: field::sec_fetch_site 映射缺失或错位");
    static_assert(to_field(field::sec_fetch_user) == http::field::sec_fetch_user,
                  "enum_conv: field::sec_fetch_user 映射缺失或错位");
    static_assert(to_field(field::sec_websocket_accept) == http::field::sec_websocket_accept,
                  "enum_conv: field::sec_websocket_accept 映射缺失或错位");
    static_assert(to_field(field::sec_websocket_extensions) == http::field::sec_websocket_extensions,
                  "enum_conv: field::sec_websocket_extensions 映射缺失或错位");
    static_assert(to_field(field::sec_websocket_key) == http::field::sec_websocket_key,
                  "enum_conv: field::sec_websocket_key 映射缺失或错位");
    static_assert(to_field(field::sec_websocket_protocol) == http::field::sec_websocket_protocol,
                  "enum_conv: field::sec_websocket_protocol 映射缺失或错位");
    static_assert(to_field(field::sec_websocket_version) == http::field::sec_websocket_version,
                  "enum_conv: field::sec_websocket_version 映射缺失或错位");
    static_assert(to_field(field::security_scheme) == http::field::security_scheme,
                  "enum_conv: field::security_scheme 映射缺失或错位");
    static_assert(to_field(field::see_also) == http::field::see_also, "enum_conv: field::see_also 映射缺失或错位");
    static_assert(to_field(field::sender) == http::field::sender, "enum_conv: field::sender 映射缺失或错位");
    static_assert(to_field(field::sensitivity) == http::field::sensitivity,
                  "enum_conv: field::sensitivity 映射缺失或错位");
    static_assert(to_field(field::server) == http::field::server, "enum_conv: field::server 映射缺失或错位");
    static_assert(to_field(field::set_cookie) == http::field::set_cookie,
                  "enum_conv: field::set_cookie 映射缺失或错位");
    static_assert(to_field(field::set_cookie2) == http::field::set_cookie2,
                  "enum_conv: field::set_cookie2 映射缺失或错位");
    static_assert(to_field(field::setprofile) == http::field::setprofile,
                  "enum_conv: field::setprofile 映射缺失或错位");
    static_assert(to_field(field::sio_label) == http::field::sio_label, "enum_conv: field::sio_label 映射缺失或错位");
    static_assert(to_field(field::sio_label_history) == http::field::sio_label_history,
                  "enum_conv: field::sio_label_history 映射缺失或错位");
    static_assert(to_field(field::slug) == http::field::slug, "enum_conv: field::slug 映射缺失或错位");
    static_assert(to_field(field::soapaction) == http::field::soapaction,
                  "enum_conv: field::soapaction 映射缺失或错位");
    static_assert(to_field(field::solicitation) == http::field::solicitation,
                  "enum_conv: field::solicitation 映射缺失或错位");
    static_assert(to_field(field::status_uri) == http::field::status_uri,
                  "enum_conv: field::status_uri 映射缺失或错位");
    static_assert(to_field(field::strict_transport_security) == http::field::strict_transport_security,
                  "enum_conv: field::strict_transport_security 映射缺失或错位");
    static_assert(to_field(field::subject) == http::field::subject, "enum_conv: field::subject 映射缺失或错位");
    static_assert(to_field(field::subok) == http::field::subok, "enum_conv: field::subok 映射缺失或错位");
    static_assert(to_field(field::subst) == http::field::subst, "enum_conv: field::subst 映射缺失或错位");
    static_assert(to_field(field::summary) == http::field::summary, "enum_conv: field::summary 映射缺失或错位");
    static_assert(to_field(field::supersedes) == http::field::supersedes,
                  "enum_conv: field::supersedes 映射缺失或错位");
    static_assert(to_field(field::surrogate_capability) == http::field::surrogate_capability,
                  "enum_conv: field::surrogate_capability 映射缺失或错位");
    static_assert(to_field(field::surrogate_control) == http::field::surrogate_control,
                  "enum_conv: field::surrogate_control 映射缺失或错位");
    static_assert(to_field(field::tcn) == http::field::tcn, "enum_conv: field::tcn 映射缺失或错位");
    static_assert(to_field(field::te) == http::field::te, "enum_conv: field::te 映射缺失或错位");
    static_assert(to_field(field::timeout) == http::field::timeout, "enum_conv: field::timeout 映射缺失或错位");
    static_assert(to_field(field::title) == http::field::title, "enum_conv: field::title 映射缺失或错位");
    static_assert(to_field(field::to) == http::field::to, "enum_conv: field::to 映射缺失或错位");
    static_assert(to_field(field::topic) == http::field::topic, "enum_conv: field::topic 映射缺失或错位");
    static_assert(to_field(field::trailer) == http::field::trailer, "enum_conv: field::trailer 映射缺失或错位");
    static_assert(to_field(field::transfer_encoding) == http::field::transfer_encoding,
                  "enum_conv: field::transfer_encoding 映射缺失或错位");
    static_assert(to_field(field::ttl) == http::field::ttl, "enum_conv: field::ttl 映射缺失或错位");
    static_assert(to_field(field::ua_color) == http::field::ua_color, "enum_conv: field::ua_color 映射缺失或错位");
    static_assert(to_field(field::ua_media) == http::field::ua_media, "enum_conv: field::ua_media 映射缺失或错位");
    static_assert(to_field(field::ua_pixels) == http::field::ua_pixels, "enum_conv: field::ua_pixels 映射缺失或错位");
    static_assert(to_field(field::ua_resolution) == http::field::ua_resolution,
                  "enum_conv: field::ua_resolution 映射缺失或错位");
    static_assert(to_field(field::ua_windowpixels) == http::field::ua_windowpixels,
                  "enum_conv: field::ua_windowpixels 映射缺失或错位");
    static_assert(to_field(field::upgrade) == http::field::upgrade, "enum_conv: field::upgrade 映射缺失或错位");
    static_assert(to_field(field::urgency) == http::field::urgency, "enum_conv: field::urgency 映射缺失或错位");
    static_assert(to_field(field::uri) == http::field::uri, "enum_conv: field::uri 映射缺失或错位");
    static_assert(to_field(field::user_agent) == http::field::user_agent,
                  "enum_conv: field::user_agent 映射缺失或错位");
    static_assert(to_field(field::variant_vary) == http::field::variant_vary,
                  "enum_conv: field::variant_vary 映射缺失或错位");
    static_assert(to_field(field::vary) == http::field::vary, "enum_conv: field::vary 映射缺失或错位");
    static_assert(to_field(field::vbr_info) == http::field::vbr_info, "enum_conv: field::vbr_info 映射缺失或错位");
    static_assert(to_field(field::version) == http::field::version, "enum_conv: field::version 映射缺失或错位");
    static_assert(to_field(field::via) == http::field::via, "enum_conv: field::via 映射缺失或错位");
    static_assert(to_field(field::want_digest) == http::field::want_digest,
                  "enum_conv: field::want_digest 映射缺失或错位");
    static_assert(to_field(field::warning) == http::field::warning, "enum_conv: field::warning 映射缺失或错位");
    static_assert(to_field(field::www_authenticate) == http::field::www_authenticate,
                  "enum_conv: field::www_authenticate 映射缺失或错位");
    static_assert(to_field(field::x_archived_at) == http::field::x_archived_at,
                  "enum_conv: field::x_archived_at 映射缺失或错位");
    static_assert(to_field(field::x_device_accept) == http::field::x_device_accept,
                  "enum_conv: field::x_device_accept 映射缺失或错位");
    static_assert(to_field(field::x_device_accept_charset) == http::field::x_device_accept_charset,
                  "enum_conv: field::x_device_accept_charset 映射缺失或错位");
    static_assert(to_field(field::x_device_accept_encoding) == http::field::x_device_accept_encoding,
                  "enum_conv: field::x_device_accept_encoding 映射缺失或错位");
    static_assert(to_field(field::x_device_accept_language) == http::field::x_device_accept_language,
                  "enum_conv: field::x_device_accept_language 映射缺失或错位");
    static_assert(to_field(field::x_device_user_agent) == http::field::x_device_user_agent,
                  "enum_conv: field::x_device_user_agent 映射缺失或错位");
    static_assert(to_field(field::x_frame_options) == http::field::x_frame_options,
                  "enum_conv: field::x_frame_options 映射缺失或错位");
    static_assert(to_field(field::x_mittente) == http::field::x_mittente,
                  "enum_conv: field::x_mittente 映射缺失或错位");
    static_assert(to_field(field::x_pgp_sig) == http::field::x_pgp_sig, "enum_conv: field::x_pgp_sig 映射缺失或错位");
    static_assert(to_field(field::x_ricevuta) == http::field::x_ricevuta,
                  "enum_conv: field::x_ricevuta 映射缺失或错位");
    static_assert(to_field(field::x_riferimento_message_id) == http::field::x_riferimento_message_id,
                  "enum_conv: field::x_riferimento_message_id 映射缺失或错位");
    static_assert(to_field(field::x_tiporicevuta) == http::field::x_tiporicevuta,
                  "enum_conv: field::x_tiporicevuta 映射缺失或错位");
    static_assert(to_field(field::x_trasporto) == http::field::x_trasporto,
                  "enum_conv: field::x_trasporto 映射缺失或错位");
    static_assert(to_field(field::x_verificasicurezza) == http::field::x_verificasicurezza,
                  "enum_conv: field::x_verificasicurezza 映射缺失或错位");
    static_assert(to_field(field::x400_content_identifier) == http::field::x400_content_identifier,
                  "enum_conv: field::x400_content_identifier 映射缺失或错位");
    static_assert(to_field(field::x400_content_return) == http::field::x400_content_return,
                  "enum_conv: field::x400_content_return 映射缺失或错位");
    static_assert(to_field(field::x400_content_type) == http::field::x400_content_type,
                  "enum_conv: field::x400_content_type 映射缺失或错位");
    static_assert(to_field(field::x400_mts_identifier) == http::field::x400_mts_identifier,
                  "enum_conv: field::x400_mts_identifier 映射缺失或错位");
    static_assert(to_field(field::x400_originator) == http::field::x400_originator,
                  "enum_conv: field::x400_originator 映射缺失或错位");
    static_assert(to_field(field::x400_received) == http::field::x400_received,
                  "enum_conv: field::x400_received 映射缺失或错位");
    static_assert(to_field(field::x400_recipients) == http::field::x400_recipients,
                  "enum_conv: field::x400_recipients 映射缺失或错位");
    static_assert(to_field(field::x400_trace) == http::field::x400_trace,
                  "enum_conv: field::x400_trace 映射缺失或错位");
    static_assert(to_field(field::xref) == http::field::xref, "enum_conv: field::xref 映射缺失或错位");

    // http::field -> field
    static_assert(to_field(http::field::unknown) == field::unknown, "enum_conv: http::field::unknown 映射缺失或错位");
    static_assert(to_field(http::field::a_im) == field::a_im, "enum_conv: http::field::a_im 映射缺失或错位");
    static_assert(to_field(http::field::accept) == field::accept, "enum_conv: http::field::accept 映射缺失或错位");
    static_assert(to_field(http::field::accept_additions) == field::accept_additions,
                  "enum_conv: http::field::accept_additions 映射缺失或错位");
    static_assert(to_field(http::field::accept_charset) == field::accept_charset,
                  "enum_conv: http::field::accept_charset 映射缺失或错位");
    static_assert(to_field(http::field::accept_datetime) == field::accept_datetime,
                  "enum_conv: http::field::accept_datetime 映射缺失或错位");
    static_assert(to_field(http::field::accept_encoding) == field::accept_encoding,
                  "enum_conv: http::field::accept_encoding 映射缺失或错位");
    static_assert(to_field(http::field::accept_features) == field::accept_features,
                  "enum_conv: http::field::accept_features 映射缺失或错位");
    static_assert(to_field(http::field::accept_language) == field::accept_language,
                  "enum_conv: http::field::accept_language 映射缺失或错位");
    static_assert(to_field(http::field::accept_patch) == field::accept_patch,
                  "enum_conv: http::field::accept_patch 映射缺失或错位");
    static_assert(to_field(http::field::accept_post) == field::accept_post,
                  "enum_conv: http::field::accept_post 映射缺失或错位");
    static_assert(to_field(http::field::accept_ranges) == field::accept_ranges,
                  "enum_conv: http::field::accept_ranges 映射缺失或错位");
    static_assert(to_field(http::field::access_control) == field::access_control,
                  "enum_conv: http::field::access_control 映射缺失或错位");
    static_assert(to_field(http::field::access_control_allow_credentials) == field::access_control_allow_credentials,
                  "enum_conv: http::field::access_control_allow_credentials 映射缺失或错位");
    static_assert(to_field(http::field::access_control_allow_headers) == field::access_control_allow_headers,
                  "enum_conv: http::field::access_control_allow_headers 映射缺失或错位");
    static_assert(to_field(http::field::access_control_allow_methods) == field::access_control_allow_methods,
                  "enum_conv: http::field::access_control_allow_methods 映射缺失或错位");
    static_assert(to_field(http::field::access_control_allow_origin) == field::access_control_allow_origin,
                  "enum_conv: http::field::access_control_allow_origin 映射缺失或错位");
    static_assert(to_field(http::field::access_control_expose_headers) == field::access_control_expose_headers,
                  "enum_conv: http::field::access_control_expose_headers 映射缺失或错位");
    static_assert(to_field(http::field::access_control_max_age) == field::access_control_max_age,
                  "enum_conv: http::field::access_control_max_age 映射缺失或错位");
    static_assert(to_field(http::field::access_control_request_headers) == field::access_control_request_headers,
                  "enum_conv: http::field::access_control_request_headers 映射缺失或错位");
    static_assert(to_field(http::field::access_control_request_method) == field::access_control_request_method,
                  "enum_conv: http::field::access_control_request_method 映射缺失或错位");
    static_assert(to_field(http::field::age) == field::age, "enum_conv: http::field::age 映射缺失或错位");
    static_assert(to_field(http::field::allow) == field::allow, "enum_conv: http::field::allow 映射缺失或错位");
    static_assert(to_field(http::field::alpn) == field::alpn, "enum_conv: http::field::alpn 映射缺失或错位");
    static_assert(to_field(http::field::also_control) == field::also_control,
                  "enum_conv: http::field::also_control 映射缺失或错位");
    static_assert(to_field(http::field::alt_svc) == field::alt_svc, "enum_conv: http::field::alt_svc 映射缺失或错位");
    static_assert(to_field(http::field::alt_used) == field::alt_used,
                  "enum_conv: http::field::alt_used 映射缺失或错位");
    static_assert(to_field(http::field::alternate_recipient) == field::alternate_recipient,
                  "enum_conv: http::field::alternate_recipient 映射缺失或错位");
    static_assert(to_field(http::field::alternates) == field::alternates,
                  "enum_conv: http::field::alternates 映射缺失或错位");
    static_assert(to_field(http::field::apparently_to) == field::apparently_to,
                  "enum_conv: http::field::apparently_to 映射缺失或错位");
    static_assert(to_field(http::field::apply_to_redirect_ref) == field::apply_to_redirect_ref,
                  "enum_conv: http::field::apply_to_redirect_ref 映射缺失或错位");
    static_assert(to_field(http::field::approved) == field::approved,
                  "enum_conv: http::field::approved 映射缺失或错位");
    static_assert(to_field(http::field::archive) == field::archive, "enum_conv: http::field::archive 映射缺失或错位");
    static_assert(to_field(http::field::archived_at) == field::archived_at,
                  "enum_conv: http::field::archived_at 映射缺失或错位");
    static_assert(to_field(http::field::article_names) == field::article_names,
                  "enum_conv: http::field::article_names 映射缺失或错位");
    static_assert(to_field(http::field::article_updates) == field::article_updates,
                  "enum_conv: http::field::article_updates 映射缺失或错位");
    static_assert(to_field(http::field::authentication_control) == field::authentication_control,
                  "enum_conv: http::field::authentication_control 映射缺失或错位");
    static_assert(to_field(http::field::authentication_info) == field::authentication_info,
                  "enum_conv: http::field::authentication_info 映射缺失或错位");
    static_assert(to_field(http::field::authentication_results) == field::authentication_results,
                  "enum_conv: http::field::authentication_results 映射缺失或错位");
    static_assert(to_field(http::field::authorization) == field::authorization,
                  "enum_conv: http::field::authorization 映射缺失或错位");
    static_assert(to_field(http::field::auto_submitted) == field::auto_submitted,
                  "enum_conv: http::field::auto_submitted 映射缺失或错位");
    static_assert(to_field(http::field::autoforwarded) == field::autoforwarded,
                  "enum_conv: http::field::autoforwarded 映射缺失或错位");
    static_assert(to_field(http::field::autosubmitted) == field::autosubmitted,
                  "enum_conv: http::field::autosubmitted 映射缺失或错位");
    static_assert(to_field(http::field::base) == field::base, "enum_conv: http::field::base 映射缺失或错位");
    static_assert(to_field(http::field::bcc) == field::bcc, "enum_conv: http::field::bcc 映射缺失或错位");
    static_assert(to_field(http::field::body) == field::body, "enum_conv: http::field::body 映射缺失或错位");
    static_assert(to_field(http::field::c_ext) == field::c_ext, "enum_conv: http::field::c_ext 映射缺失或错位");
    static_assert(to_field(http::field::c_man) == field::c_man, "enum_conv: http::field::c_man 映射缺失或错位");
    static_assert(to_field(http::field::c_opt) == field::c_opt, "enum_conv: http::field::c_opt 映射缺失或错位");
    static_assert(to_field(http::field::c_pep) == field::c_pep, "enum_conv: http::field::c_pep 映射缺失或错位");
    static_assert(to_field(http::field::c_pep_info) == field::c_pep_info,
                  "enum_conv: http::field::c_pep_info 映射缺失或错位");
    static_assert(to_field(http::field::cache_control) == field::cache_control,
                  "enum_conv: http::field::cache_control 映射缺失或错位");
    static_assert(to_field(http::field::caldav_timezones) == field::caldav_timezones,
                  "enum_conv: http::field::caldav_timezones 映射缺失或错位");
    static_assert(to_field(http::field::cancel_key) == field::cancel_key,
                  "enum_conv: http::field::cancel_key 映射缺失或错位");
    static_assert(to_field(http::field::cancel_lock) == field::cancel_lock,
                  "enum_conv: http::field::cancel_lock 映射缺失或错位");
    static_assert(to_field(http::field::cc) == field::cc, "enum_conv: http::field::cc 映射缺失或错位");
    static_assert(to_field(http::field::close) == field::close, "enum_conv: http::field::close 映射缺失或错位");
    static_assert(to_field(http::field::comments) == field::comments,
                  "enum_conv: http::field::comments 映射缺失或错位");
    static_assert(to_field(http::field::compliance) == field::compliance,
                  "enum_conv: http::field::compliance 映射缺失或错位");
    static_assert(to_field(http::field::connection) == field::connection,
                  "enum_conv: http::field::connection 映射缺失或错位");
    static_assert(to_field(http::field::content_alternative) == field::content_alternative,
                  "enum_conv: http::field::content_alternative 映射缺失或错位");
    static_assert(to_field(http::field::content_base) == field::content_base,
                  "enum_conv: http::field::content_base 映射缺失或错位");
    static_assert(to_field(http::field::content_description) == field::content_description,
                  "enum_conv: http::field::content_description 映射缺失或错位");
    static_assert(to_field(http::field::content_disposition) == field::content_disposition,
                  "enum_conv: http::field::content_disposition 映射缺失或错位");
    static_assert(to_field(http::field::content_duration) == field::content_duration,
                  "enum_conv: http::field::content_duration 映射缺失或错位");
    static_assert(to_field(http::field::content_encoding) == field::content_encoding,
                  "enum_conv: http::field::content_encoding 映射缺失或错位");
    static_assert(to_field(http::field::content_features) == field::content_features,
                  "enum_conv: http::field::content_features 映射缺失或错位");
    static_assert(to_field(http::field::content_id) == field::content_id,
                  "enum_conv: http::field::content_id 映射缺失或错位");
    static_assert(to_field(http::field::content_identifier) == field::content_identifier,
                  "enum_conv: http::field::content_identifier 映射缺失或错位");
    static_assert(to_field(http::field::content_language) == field::content_language,
                  "enum_conv: http::field::content_language 映射缺失或错位");
    static_assert(to_field(http::field::content_length) == field::content_length,
                  "enum_conv: http::field::content_length 映射缺失或错位");
    static_assert(to_field(http::field::content_location) == field::content_location,
                  "enum_conv: http::field::content_location 映射缺失或错位");
    static_assert(to_field(http::field::content_md5) == field::content_md5,
                  "enum_conv: http::field::content_md5 映射缺失或错位");
    static_assert(to_field(http::field::content_range) == field::content_range,
                  "enum_conv: http::field::content_range 映射缺失或错位");
    static_assert(to_field(http::field::content_return) == field::content_return,
                  "enum_conv: http::field::content_return 映射缺失或错位");
    static_assert(to_field(http::field::content_script_type) == field::content_script_type,
                  "enum_conv: http::field::content_script_type 映射缺失或错位");
    static_assert(to_field(http::field::content_style_type) == field::content_style_type,
                  "enum_conv: http::field::content_style_type 映射缺失或错位");
    static_assert(to_field(http::field::content_transfer_encoding) == field::content_transfer_encoding,
                  "enum_conv: http::field::content_transfer_encoding 映射缺失或错位");
    static_assert(to_field(http::field::content_type) == field::content_type,
                  "enum_conv: http::field::content_type 映射缺失或错位");
    static_assert(to_field(http::field::content_version) == field::content_version,
                  "enum_conv: http::field::content_version 映射缺失或错位");
    static_assert(to_field(http::field::control) == field::control, "enum_conv: http::field::control 映射缺失或错位");
    static_assert(to_field(http::field::conversion) == field::conversion,
                  "enum_conv: http::field::conversion 映射缺失或错位");
    static_assert(to_field(http::field::conversion_with_loss) == field::conversion_with_loss,
                  "enum_conv: http::field::conversion_with_loss 映射缺失或错位");
    static_assert(to_field(http::field::cookie) == field::cookie, "enum_conv: http::field::cookie 映射缺失或错位");
    static_assert(to_field(http::field::cookie2) == field::cookie2, "enum_conv: http::field::cookie2 映射缺失或错位");
    static_assert(to_field(http::field::cost) == field::cost, "enum_conv: http::field::cost 映射缺失或错位");
    static_assert(to_field(http::field::dasl) == field::dasl, "enum_conv: http::field::dasl 映射缺失或错位");
    static_assert(to_field(http::field::date) == field::date, "enum_conv: http::field::date 映射缺失或错位");
    static_assert(to_field(http::field::date_received) == field::date_received,
                  "enum_conv: http::field::date_received 映射缺失或错位");
    static_assert(to_field(http::field::dav) == field::dav, "enum_conv: http::field::dav 映射缺失或错位");
    static_assert(to_field(http::field::default_style) == field::default_style,
                  "enum_conv: http::field::default_style 映射缺失或错位");
    static_assert(to_field(http::field::deferred_delivery) == field::deferred_delivery,
                  "enum_conv: http::field::deferred_delivery 映射缺失或错位");
    static_assert(to_field(http::field::delivery_date) == field::delivery_date,
                  "enum_conv: http::field::delivery_date 映射缺失或错位");
    static_assert(to_field(http::field::delta_base) == field::delta_base,
                  "enum_conv: http::field::delta_base 映射缺失或错位");
    static_assert(to_field(http::field::depth) == field::depth, "enum_conv: http::field::depth 映射缺失或错位");
    static_assert(to_field(http::field::derived_from) == field::derived_from,
                  "enum_conv: http::field::derived_from 映射缺失或错位");
    static_assert(to_field(http::field::destination) == field::destination,
                  "enum_conv: http::field::destination 映射缺失或错位");
    static_assert(to_field(http::field::differential_id) == field::differential_id,
                  "enum_conv: http::field::differential_id 映射缺失或错位");
    static_assert(to_field(http::field::digest) == field::digest, "enum_conv: http::field::digest 映射缺失或错位");
    static_assert(to_field(http::field::discarded_x400_ipms_extensions) == field::discarded_x400_ipms_extensions,
                  "enum_conv: http::field::discarded_x400_ipms_extensions 映射缺失或错位");
    static_assert(to_field(http::field::discarded_x400_mts_extensions) == field::discarded_x400_mts_extensions,
                  "enum_conv: http::field::discarded_x400_mts_extensions 映射缺失或错位");
    static_assert(to_field(http::field::disclose_recipients) == field::disclose_recipients,
                  "enum_conv: http::field::disclose_recipients 映射缺失或错位");
    static_assert(to_field(http::field::disposition_notification_options) == field::disposition_notification_options,
                  "enum_conv: http::field::disposition_notification_options 映射缺失或错位");
    static_assert(to_field(http::field::disposition_notification_to) == field::disposition_notification_to,
                  "enum_conv: http::field::disposition_notification_to 映射缺失或错位");
    static_assert(to_field(http::field::distribution) == field::distribution,
                  "enum_conv: http::field::distribution 映射缺失或错位");
    static_assert(to_field(http::field::dkim_signature) == field::dkim_signature,
                  "enum_conv: http::field::dkim_signature 映射缺失或错位");
    static_assert(to_field(http::field::dl_expansion_history) == field::dl_expansion_history,
                  "enum_conv: http::field::dl_expansion_history 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_bcc) == field::downgraded_bcc,
                  "enum_conv: http::field::downgraded_bcc 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_cc) == field::downgraded_cc,
                  "enum_conv: http::field::downgraded_cc 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_disposition_notification_to)
                      == field::downgraded_disposition_notification_to,
                  "enum_conv: http::field::downgraded_disposition_notification_to 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_final_recipient) == field::downgraded_final_recipient,
                  "enum_conv: http::field::downgraded_final_recipient 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_from) == field::downgraded_from,
                  "enum_conv: http::field::downgraded_from 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_in_reply_to) == field::downgraded_in_reply_to,
                  "enum_conv: http::field::downgraded_in_reply_to 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_mail_from) == field::downgraded_mail_from,
                  "enum_conv: http::field::downgraded_mail_from 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_message_id) == field::downgraded_message_id,
                  "enum_conv: http::field::downgraded_message_id 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_original_recipient) == field::downgraded_original_recipient,
                  "enum_conv: http::field::downgraded_original_recipient 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_rcpt_to) == field::downgraded_rcpt_to,
                  "enum_conv: http::field::downgraded_rcpt_to 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_references) == field::downgraded_references,
                  "enum_conv: http::field::downgraded_references 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_reply_to) == field::downgraded_reply_to,
                  "enum_conv: http::field::downgraded_reply_to 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_resent_bcc) == field::downgraded_resent_bcc,
                  "enum_conv: http::field::downgraded_resent_bcc 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_resent_cc) == field::downgraded_resent_cc,
                  "enum_conv: http::field::downgraded_resent_cc 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_resent_from) == field::downgraded_resent_from,
                  "enum_conv: http::field::downgraded_resent_from 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_resent_reply_to) == field::downgraded_resent_reply_to,
                  "enum_conv: http::field::downgraded_resent_reply_to 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_resent_sender) == field::downgraded_resent_sender,
                  "enum_conv: http::field::downgraded_resent_sender 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_resent_to) == field::downgraded_resent_to,
                  "enum_conv: http::field::downgraded_resent_to 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_return_path) == field::downgraded_return_path,
                  "enum_conv: http::field::downgraded_return_path 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_sender) == field::downgraded_sender,
                  "enum_conv: http::field::downgraded_sender 映射缺失或错位");
    static_assert(to_field(http::field::downgraded_to) == field::downgraded_to,
                  "enum_conv: http::field::downgraded_to 映射缺失或错位");
    static_assert(to_field(http::field::ediint_features) == field::ediint_features,
                  "enum_conv: http::field::ediint_features 映射缺失或错位");
    static_assert(to_field(http::field::eesst_version) == field::eesst_version,
                  "enum_conv: http::field::eesst_version 映射缺失或错位");
    static_assert(to_field(http::field::encoding) == field::encoding,
                  "enum_conv: http::field::encoding 映射缺失或错位");
    static_assert(to_field(http::field::encrypted) == field::encrypted,
                  "enum_conv: http::field::encrypted 映射缺失或错位");
    static_assert(to_field(http::field::errors_to) == field::errors_to,
                  "enum_conv: http::field::errors_to 映射缺失或错位");
    static_assert(to_field(http::field::etag) == field::etag, "enum_conv: http::field::etag 映射缺失或错位");
    static_assert(to_field(http::field::expect) == field::expect, "enum_conv: http::field::expect 映射缺失或错位");
    static_assert(to_field(http::field::expires) == field::expires, "enum_conv: http::field::expires 映射缺失或错位");
    static_assert(to_field(http::field::expiry_date) == field::expiry_date,
                  "enum_conv: http::field::expiry_date 映射缺失或错位");
    static_assert(to_field(http::field::ext) == field::ext, "enum_conv: http::field::ext 映射缺失或错位");
    static_assert(to_field(http::field::followup_to) == field::followup_to,
                  "enum_conv: http::field::followup_to 映射缺失或错位");
    static_assert(to_field(http::field::forwarded) == field::forwarded,
                  "enum_conv: http::field::forwarded 映射缺失或错位");
    static_assert(to_field(http::field::from) == field::from, "enum_conv: http::field::from 映射缺失或错位");
    static_assert(to_field(http::field::generate_delivery_report) == field::generate_delivery_report,
                  "enum_conv: http::field::generate_delivery_report 映射缺失或错位");
    static_assert(to_field(http::field::getprofile) == field::getprofile,
                  "enum_conv: http::field::getprofile 映射缺失或错位");
    static_assert(to_field(http::field::hobareg) == field::hobareg, "enum_conv: http::field::hobareg 映射缺失或错位");
    static_assert(to_field(http::field::host) == field::host, "enum_conv: http::field::host 映射缺失或错位");
    static_assert(to_field(http::field::http2_settings) == field::http2_settings,
                  "enum_conv: http::field::http2_settings 映射缺失或错位");
    static_assert(to_field(http::field::if_) == field::if_, "enum_conv: http::field::if_ 映射缺失或错位");
    static_assert(to_field(http::field::if_match) == field::if_match,
                  "enum_conv: http::field::if_match 映射缺失或错位");
    static_assert(to_field(http::field::if_modified_since) == field::if_modified_since,
                  "enum_conv: http::field::if_modified_since 映射缺失或错位");
    static_assert(to_field(http::field::if_none_match) == field::if_none_match,
                  "enum_conv: http::field::if_none_match 映射缺失或错位");
    static_assert(to_field(http::field::if_range) == field::if_range,
                  "enum_conv: http::field::if_range 映射缺失或错位");
    static_assert(to_field(http::field::if_schedule_tag_match) == field::if_schedule_tag_match,
                  "enum_conv: http::field::if_schedule_tag_match 映射缺失或错位");
    static_assert(to_field(http::field::if_unmodified_since) == field::if_unmodified_since,
                  "enum_conv: http::field::if_unmodified_since 映射缺失或错位");
    static_assert(to_field(http::field::im) == field::im, "enum_conv: http::field::im 映射缺失或错位");
    static_assert(to_field(http::field::importance) == field::importance,
                  "enum_conv: http::field::importance 映射缺失或错位");
    static_assert(to_field(http::field::in_reply_to) == field::in_reply_to,
                  "enum_conv: http::field::in_reply_to 映射缺失或错位");
    static_assert(to_field(http::field::incomplete_copy) == field::incomplete_copy,
                  "enum_conv: http::field::incomplete_copy 映射缺失或错位");
    static_assert(to_field(http::field::injection_date) == field::injection_date,
                  "enum_conv: http::field::injection_date 映射缺失或错位");
    static_assert(to_field(http::field::injection_info) == field::injection_info,
                  "enum_conv: http::field::injection_info 映射缺失或错位");
    static_assert(to_field(http::field::jabber_id) == field::jabber_id,
                  "enum_conv: http::field::jabber_id 映射缺失或错位");
    static_assert(to_field(http::field::keep_alive) == field::keep_alive,
                  "enum_conv: http::field::keep_alive 映射缺失或错位");
    static_assert(to_field(http::field::keywords) == field::keywords,
                  "enum_conv: http::field::keywords 映射缺失或错位");
    static_assert(to_field(http::field::label) == field::label, "enum_conv: http::field::label 映射缺失或错位");
    static_assert(to_field(http::field::language) == field::language,
                  "enum_conv: http::field::language 映射缺失或错位");
    static_assert(to_field(http::field::last_modified) == field::last_modified,
                  "enum_conv: http::field::last_modified 映射缺失或错位");
    static_assert(to_field(http::field::latest_delivery_time) == field::latest_delivery_time,
                  "enum_conv: http::field::latest_delivery_time 映射缺失或错位");
    static_assert(to_field(http::field::lines) == field::lines, "enum_conv: http::field::lines 映射缺失或错位");
    static_assert(to_field(http::field::link) == field::link, "enum_conv: http::field::link 映射缺失或错位");
    static_assert(to_field(http::field::list_archive) == field::list_archive,
                  "enum_conv: http::field::list_archive 映射缺失或错位");
    static_assert(to_field(http::field::list_help) == field::list_help,
                  "enum_conv: http::field::list_help 映射缺失或错位");
    static_assert(to_field(http::field::list_id) == field::list_id, "enum_conv: http::field::list_id 映射缺失或错位");
    static_assert(to_field(http::field::list_owner) == field::list_owner,
                  "enum_conv: http::field::list_owner 映射缺失或错位");
    static_assert(to_field(http::field::list_post) == field::list_post,
                  "enum_conv: http::field::list_post 映射缺失或错位");
    static_assert(to_field(http::field::list_subscribe) == field::list_subscribe,
                  "enum_conv: http::field::list_subscribe 映射缺失或错位");
    static_assert(to_field(http::field::list_unsubscribe) == field::list_unsubscribe,
                  "enum_conv: http::field::list_unsubscribe 映射缺失或错位");
    static_assert(to_field(http::field::list_unsubscribe_post) == field::list_unsubscribe_post,
                  "enum_conv: http::field::list_unsubscribe_post 映射缺失或错位");
    static_assert(to_field(http::field::location) == field::location,
                  "enum_conv: http::field::location 映射缺失或错位");
    static_assert(to_field(http::field::lock_token) == field::lock_token,
                  "enum_conv: http::field::lock_token 映射缺失或错位");
    static_assert(to_field(http::field::man) == field::man, "enum_conv: http::field::man 映射缺失或错位");
    static_assert(to_field(http::field::max_forwards) == field::max_forwards,
                  "enum_conv: http::field::max_forwards 映射缺失或错位");
    static_assert(to_field(http::field::memento_datetime) == field::memento_datetime,
                  "enum_conv: http::field::memento_datetime 映射缺失或错位");
    static_assert(to_field(http::field::message_context) == field::message_context,
                  "enum_conv: http::field::message_context 映射缺失或错位");
    static_assert(to_field(http::field::message_id) == field::message_id,
                  "enum_conv: http::field::message_id 映射缺失或错位");
    static_assert(to_field(http::field::message_type) == field::message_type,
                  "enum_conv: http::field::message_type 映射缺失或错位");
    static_assert(to_field(http::field::meter) == field::meter, "enum_conv: http::field::meter 映射缺失或错位");
    static_assert(to_field(http::field::method_check) == field::method_check,
                  "enum_conv: http::field::method_check 映射缺失或错位");
    static_assert(to_field(http::field::method_check_expires) == field::method_check_expires,
                  "enum_conv: http::field::method_check_expires 映射缺失或错位");
    static_assert(to_field(http::field::mime_version) == field::mime_version,
                  "enum_conv: http::field::mime_version 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_acp127_message_identifier) == field::mmhs_acp127_message_identifier,
                  "enum_conv: http::field::mmhs_acp127_message_identifier 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_authorizing_users) == field::mmhs_authorizing_users,
                  "enum_conv: http::field::mmhs_authorizing_users 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_codress_message_indicator) == field::mmhs_codress_message_indicator,
                  "enum_conv: http::field::mmhs_codress_message_indicator 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_copy_precedence) == field::mmhs_copy_precedence,
                  "enum_conv: http::field::mmhs_copy_precedence 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_exempted_address) == field::mmhs_exempted_address,
                  "enum_conv: http::field::mmhs_exempted_address 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_extended_authorisation_info) == field::mmhs_extended_authorisation_info,
                  "enum_conv: http::field::mmhs_extended_authorisation_info 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_handling_instructions) == field::mmhs_handling_instructions,
                  "enum_conv: http::field::mmhs_handling_instructions 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_message_instructions) == field::mmhs_message_instructions,
                  "enum_conv: http::field::mmhs_message_instructions 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_message_type) == field::mmhs_message_type,
                  "enum_conv: http::field::mmhs_message_type 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_originator_plad) == field::mmhs_originator_plad,
                  "enum_conv: http::field::mmhs_originator_plad 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_originator_reference) == field::mmhs_originator_reference,
                  "enum_conv: http::field::mmhs_originator_reference 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_other_recipients_indicator_cc)
                      == field::mmhs_other_recipients_indicator_cc,
                  "enum_conv: http::field::mmhs_other_recipients_indicator_cc 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_other_recipients_indicator_to)
                      == field::mmhs_other_recipients_indicator_to,
                  "enum_conv: http::field::mmhs_other_recipients_indicator_to 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_primary_precedence) == field::mmhs_primary_precedence,
                  "enum_conv: http::field::mmhs_primary_precedence 映射缺失或错位");
    static_assert(to_field(http::field::mmhs_subject_indicator_codes) == field::mmhs_subject_indicator_codes,
                  "enum_conv: http::field::mmhs_subject_indicator_codes 映射缺失或错位");
    static_assert(to_field(http::field::mt_priority) == field::mt_priority,
                  "enum_conv: http::field::mt_priority 映射缺失或错位");
    static_assert(to_field(http::field::negotiate) == field::negotiate,
                  "enum_conv: http::field::negotiate 映射缺失或错位");
    static_assert(to_field(http::field::newsgroups) == field::newsgroups,
                  "enum_conv: http::field::newsgroups 映射缺失或错位");
    static_assert(to_field(http::field::nntp_posting_date) == field::nntp_posting_date,
                  "enum_conv: http::field::nntp_posting_date 映射缺失或错位");
    static_assert(to_field(http::field::nntp_posting_host) == field::nntp_posting_host,
                  "enum_conv: http::field::nntp_posting_host 映射缺失或错位");
    static_assert(to_field(http::field::non_compliance) == field::non_compliance,
                  "enum_conv: http::field::non_compliance 映射缺失或错位");
    static_assert(to_field(http::field::obsoletes) == field::obsoletes,
                  "enum_conv: http::field::obsoletes 映射缺失或错位");
    static_assert(to_field(http::field::opt) == field::opt, "enum_conv: http::field::opt 映射缺失或错位");
    static_assert(to_field(http::field::optional) == field::optional,
                  "enum_conv: http::field::optional 映射缺失或错位");
    static_assert(to_field(http::field::optional_www_authenticate) == field::optional_www_authenticate,
                  "enum_conv: http::field::optional_www_authenticate 映射缺失或错位");
    static_assert(to_field(http::field::ordering_type) == field::ordering_type,
                  "enum_conv: http::field::ordering_type 映射缺失或错位");
    static_assert(to_field(http::field::organization) == field::organization,
                  "enum_conv: http::field::organization 映射缺失或错位");
    static_assert(to_field(http::field::origin) == field::origin, "enum_conv: http::field::origin 映射缺失或错位");
    static_assert(to_field(http::field::original_encoded_information_types)
                      == field::original_encoded_information_types,
                  "enum_conv: http::field::original_encoded_information_types 映射缺失或错位");
    static_assert(to_field(http::field::original_from) == field::original_from,
                  "enum_conv: http::field::original_from 映射缺失或错位");
    static_assert(to_field(http::field::original_message_id) == field::original_message_id,
                  "enum_conv: http::field::original_message_id 映射缺失或错位");
    static_assert(to_field(http::field::original_recipient) == field::original_recipient,
                  "enum_conv: http::field::original_recipient 映射缺失或错位");
    static_assert(to_field(http::field::original_sender) == field::original_sender,
                  "enum_conv: http::field::original_sender 映射缺失或错位");
    static_assert(to_field(http::field::original_subject) == field::original_subject,
                  "enum_conv: http::field::original_subject 映射缺失或错位");
    static_assert(to_field(http::field::originator_return_address) == field::originator_return_address,
                  "enum_conv: http::field::originator_return_address 映射缺失或错位");
    static_assert(to_field(http::field::overwrite) == field::overwrite,
                  "enum_conv: http::field::overwrite 映射缺失或错位");
    static_assert(to_field(http::field::p3p) == field::p3p, "enum_conv: http::field::p3p 映射缺失或错位");
    static_assert(to_field(http::field::path) == field::path, "enum_conv: http::field::path 映射缺失或错位");
    static_assert(to_field(http::field::pep) == field::pep, "enum_conv: http::field::pep 映射缺失或错位");
    static_assert(to_field(http::field::pep_info) == field::pep_info,
                  "enum_conv: http::field::pep_info 映射缺失或错位");
    static_assert(to_field(http::field::pics_label) == field::pics_label,
                  "enum_conv: http::field::pics_label 映射缺失或错位");
    static_assert(to_field(http::field::position) == field::position,
                  "enum_conv: http::field::position 映射缺失或错位");
    static_assert(to_field(http::field::posting_version) == field::posting_version,
                  "enum_conv: http::field::posting_version 映射缺失或错位");
    static_assert(to_field(http::field::pragma) == field::pragma, "enum_conv: http::field::pragma 映射缺失或错位");
    static_assert(to_field(http::field::prefer) == field::prefer, "enum_conv: http::field::prefer 映射缺失或错位");
    static_assert(to_field(http::field::preference_applied) == field::preference_applied,
                  "enum_conv: http::field::preference_applied 映射缺失或错位");
    static_assert(to_field(http::field::prevent_nondelivery_report) == field::prevent_nondelivery_report,
                  "enum_conv: http::field::prevent_nondelivery_report 映射缺失或错位");
    static_assert(to_field(http::field::priority) == field::priority,
                  "enum_conv: http::field::priority 映射缺失或错位");
    static_assert(to_field(http::field::privicon) == field::privicon,
                  "enum_conv: http::field::privicon 映射缺失或错位");
    static_assert(to_field(http::field::profileobject) == field::profileobject,
                  "enum_conv: http::field::profileobject 映射缺失或错位");
    static_assert(to_field(http::field::protocol) == field::protocol,
                  "enum_conv: http::field::protocol 映射缺失或错位");
    static_assert(to_field(http::field::protocol_info) == field::protocol_info,
                  "enum_conv: http::field::protocol_info 映射缺失或错位");
    static_assert(to_field(http::field::protocol_query) == field::protocol_query,
                  "enum_conv: http::field::protocol_query 映射缺失或错位");
    static_assert(to_field(http::field::protocol_request) == field::protocol_request,
                  "enum_conv: http::field::protocol_request 映射缺失或错位");
    static_assert(to_field(http::field::proxy_authenticate) == field::proxy_authenticate,
                  "enum_conv: http::field::proxy_authenticate 映射缺失或错位");
    static_assert(to_field(http::field::proxy_authentication_info) == field::proxy_authentication_info,
                  "enum_conv: http::field::proxy_authentication_info 映射缺失或错位");
    static_assert(to_field(http::field::proxy_authorization) == field::proxy_authorization,
                  "enum_conv: http::field::proxy_authorization 映射缺失或错位");
    static_assert(to_field(http::field::proxy_connection) == field::proxy_connection,
                  "enum_conv: http::field::proxy_connection 映射缺失或错位");
    static_assert(to_field(http::field::proxy_features) == field::proxy_features,
                  "enum_conv: http::field::proxy_features 映射缺失或错位");
    static_assert(to_field(http::field::proxy_instruction) == field::proxy_instruction,
                  "enum_conv: http::field::proxy_instruction 映射缺失或错位");
    static_assert(to_field(http::field::public_) == field::public_, "enum_conv: http::field::public_ 映射缺失或错位");
    static_assert(to_field(http::field::public_key_pins) == field::public_key_pins,
                  "enum_conv: http::field::public_key_pins 映射缺失或错位");
    static_assert(to_field(http::field::public_key_pins_report_only) == field::public_key_pins_report_only,
                  "enum_conv: http::field::public_key_pins_report_only 映射缺失或错位");
    static_assert(to_field(http::field::range) == field::range, "enum_conv: http::field::range 映射缺失或错位");
    static_assert(to_field(http::field::received) == field::received,
                  "enum_conv: http::field::received 映射缺失或错位");
    static_assert(to_field(http::field::received_spf) == field::received_spf,
                  "enum_conv: http::field::received_spf 映射缺失或错位");
    static_assert(to_field(http::field::redirect_ref) == field::redirect_ref,
                  "enum_conv: http::field::redirect_ref 映射缺失或错位");
    static_assert(to_field(http::field::references) == field::references,
                  "enum_conv: http::field::references 映射缺失或错位");
    static_assert(to_field(http::field::referer) == field::referer, "enum_conv: http::field::referer 映射缺失或错位");
    static_assert(to_field(http::field::referer_root) == field::referer_root,
                  "enum_conv: http::field::referer_root 映射缺失或错位");
    static_assert(to_field(http::field::relay_version) == field::relay_version,
                  "enum_conv: http::field::relay_version 映射缺失或错位");
    static_assert(to_field(http::field::reply_by) == field::reply_by,
                  "enum_conv: http::field::reply_by 映射缺失或错位");
    static_assert(to_field(http::field::reply_to) == field::reply_to,
                  "enum_conv: http::field::reply_to 映射缺失或错位");
    static_assert(to_field(http::field::require_recipient_valid_since) == field::require_recipient_valid_since,
                  "enum_conv: http::field::require_recipient_valid_since 映射缺失或错位");
    static_assert(to_field(http::field::resent_bcc) == field::resent_bcc,
                  "enum_conv: http::field::resent_bcc 映射缺失或错位");
    static_assert(to_field(http::field::resent_cc) == field::resent_cc,
                  "enum_conv: http::field::resent_cc 映射缺失或错位");
    static_assert(to_field(http::field::resent_date) == field::resent_date,
                  "enum_conv: http::field::resent_date 映射缺失或错位");
    static_assert(to_field(http::field::resent_from) == field::resent_from,
                  "enum_conv: http::field::resent_from 映射缺失或错位");
    static_assert(to_field(http::field::resent_message_id) == field::resent_message_id,
                  "enum_conv: http::field::resent_message_id 映射缺失或错位");
    static_assert(to_field(http::field::resent_reply_to) == field::resent_reply_to,
                  "enum_conv: http::field::resent_reply_to 映射缺失或错位");
    static_assert(to_field(http::field::resent_sender) == field::resent_sender,
                  "enum_conv: http::field::resent_sender 映射缺失或错位");
    static_assert(to_field(http::field::resent_to) == field::resent_to,
                  "enum_conv: http::field::resent_to 映射缺失或错位");
    static_assert(to_field(http::field::resolution_hint) == field::resolution_hint,
                  "enum_conv: http::field::resolution_hint 映射缺失或错位");
    static_assert(to_field(http::field::resolver_location) == field::resolver_location,
                  "enum_conv: http::field::resolver_location 映射缺失或错位");
    static_assert(to_field(http::field::retry_after) == field::retry_after,
                  "enum_conv: http::field::retry_after 映射缺失或错位");
    static_assert(to_field(http::field::return_path) == field::return_path,
                  "enum_conv: http::field::return_path 映射缺失或错位");
    static_assert(to_field(http::field::safe) == field::safe, "enum_conv: http::field::safe 映射缺失或错位");
    static_assert(to_field(http::field::schedule_reply) == field::schedule_reply,
                  "enum_conv: http::field::schedule_reply 映射缺失或错位");
    static_assert(to_field(http::field::schedule_tag) == field::schedule_tag,
                  "enum_conv: http::field::schedule_tag 映射缺失或错位");
    static_assert(to_field(http::field::sec_fetch_dest) == field::sec_fetch_dest,
                  "enum_conv: http::field::sec_fetch_dest 映射缺失或错位");
    static_assert(to_field(http::field::sec_fetch_mode) == field::sec_fetch_mode,
                  "enum_conv: http::field::sec_fetch_mode 映射缺失或错位");
    static_assert(to_field(http::field::sec_fetch_site) == field::sec_fetch_site,
                  "enum_conv: http::field::sec_fetch_site 映射缺失或错位");
    static_assert(to_field(http::field::sec_fetch_user) == field::sec_fetch_user,
                  "enum_conv: http::field::sec_fetch_user 映射缺失或错位");
    static_assert(to_field(http::field::sec_websocket_accept) == field::sec_websocket_accept,
                  "enum_conv: http::field::sec_websocket_accept 映射缺失或错位");
    static_assert(to_field(http::field::sec_websocket_extensions) == field::sec_websocket_extensions,
                  "enum_conv: http::field::sec_websocket_extensions 映射缺失或错位");
    static_assert(to_field(http::field::sec_websocket_key) == field::sec_websocket_key,
                  "enum_conv: http::field::sec_websocket_key 映射缺失或错位");
    static_assert(to_field(http::field::sec_websocket_protocol) == field::sec_websocket_protocol,
                  "enum_conv: http::field::sec_websocket_protocol 映射缺失或错位");
    static_assert(to_field(http::field::sec_websocket_version) == field::sec_websocket_version,
                  "enum_conv: http::field::sec_websocket_version 映射缺失或错位");
    static_assert(to_field(http::field::security_scheme) == field::security_scheme,
                  "enum_conv: http::field::security_scheme 映射缺失或错位");
    static_assert(to_field(http::field::see_also) == field::see_also,
                  "enum_conv: http::field::see_also 映射缺失或错位");
    static_assert(to_field(http::field::sender) == field::sender, "enum_conv: http::field::sender 映射缺失或错位");
    static_assert(to_field(http::field::sensitivity) == field::sensitivity,
                  "enum_conv: http::field::sensitivity 映射缺失或错位");
    static_assert(to_field(http::field::server) == field::server, "enum_conv: http::field::server 映射缺失或错位");
    static_assert(to_field(http::field::set_cookie) == field::set_cookie,
                  "enum_conv: http::field::set_cookie 映射缺失或错位");
    static_assert(to_field(http::field::set_cookie2) == field::set_cookie2,
                  "enum_conv: http::field::set_cookie2 映射缺失或错位");
    static_assert(to_field(http::field::setprofile) == field::setprofile,
                  "enum_conv: http::field::setprofile 映射缺失或错位");
    static_assert(to_field(http::field::sio_label) == field::sio_label,
                  "enum_conv: http::field::sio_label 映射缺失或错位");
    static_assert(to_field(http::field::sio_label_history) == field::sio_label_history,
                  "enum_conv: http::field::sio_label_history 映射缺失或错位");
    static_assert(to_field(http::field::slug) == field::slug, "enum_conv: http::field::slug 映射缺失或错位");
    static_assert(to_field(http::field::soapaction) == field::soapaction,
                  "enum_conv: http::field::soapaction 映射缺失或错位");
    static_assert(to_field(http::field::solicitation) == field::solicitation,
                  "enum_conv: http::field::solicitation 映射缺失或错位");
    static_assert(to_field(http::field::status_uri) == field::status_uri,
                  "enum_conv: http::field::status_uri 映射缺失或错位");
    static_assert(to_field(http::field::strict_transport_security) == field::strict_transport_security,
                  "enum_conv: http::field::strict_transport_security 映射缺失或错位");
    static_assert(to_field(http::field::subject) == field::subject, "enum_conv: http::field::subject 映射缺失或错位");
    static_assert(to_field(http::field::subok) == field::subok, "enum_conv: http::field::subok 映射缺失或错位");
    static_assert(to_field(http::field::subst) == field::subst, "enum_conv: http::field::subst 映射缺失或错位");
    static_assert(to_field(http::field::summary) == field::summary, "enum_conv: http::field::summary 映射缺失或错位");
    static_assert(to_field(http::field::supersedes) == field::supersedes,
                  "enum_conv: http::field::supersedes 映射缺失或错位");
    static_assert(to_field(http::field::surrogate_capability) == field::surrogate_capability,
                  "enum_conv: http::field::surrogate_capability 映射缺失或错位");
    static_assert(to_field(http::field::surrogate_control) == field::surrogate_control,
                  "enum_conv: http::field::surrogate_control 映射缺失或错位");
    static_assert(to_field(http::field::tcn) == field::tcn, "enum_conv: http::field::tcn 映射缺失或错位");
    static_assert(to_field(http::field::te) == field::te, "enum_conv: http::field::te 映射缺失或错位");
    static_assert(to_field(http::field::timeout) == field::timeout, "enum_conv: http::field::timeout 映射缺失或错位");
    static_assert(to_field(http::field::title) == field::title, "enum_conv: http::field::title 映射缺失或错位");
    static_assert(to_field(http::field::to) == field::to, "enum_conv: http::field::to 映射缺失或错位");
    static_assert(to_field(http::field::topic) == field::topic, "enum_conv: http::field::topic 映射缺失或错位");
    static_assert(to_field(http::field::trailer) == field::trailer, "enum_conv: http::field::trailer 映射缺失或错位");
    static_assert(to_field(http::field::transfer_encoding) == field::transfer_encoding,
                  "enum_conv: http::field::transfer_encoding 映射缺失或错位");
    static_assert(to_field(http::field::ttl) == field::ttl, "enum_conv: http::field::ttl 映射缺失或错位");
    static_assert(to_field(http::field::ua_color) == field::ua_color,
                  "enum_conv: http::field::ua_color 映射缺失或错位");
    static_assert(to_field(http::field::ua_media) == field::ua_media,
                  "enum_conv: http::field::ua_media 映射缺失或错位");
    static_assert(to_field(http::field::ua_pixels) == field::ua_pixels,
                  "enum_conv: http::field::ua_pixels 映射缺失或错位");
    static_assert(to_field(http::field::ua_resolution) == field::ua_resolution,
                  "enum_conv: http::field::ua_resolution 映射缺失或错位");
    static_assert(to_field(http::field::ua_windowpixels) == field::ua_windowpixels,
                  "enum_conv: http::field::ua_windowpixels 映射缺失或错位");
    static_assert(to_field(http::field::upgrade) == field::upgrade, "enum_conv: http::field::upgrade 映射缺失或错位");
    static_assert(to_field(http::field::urgency) == field::urgency, "enum_conv: http::field::urgency 映射缺失或错位");
    static_assert(to_field(http::field::uri) == field::uri, "enum_conv: http::field::uri 映射缺失或错位");
    static_assert(to_field(http::field::user_agent) == field::user_agent,
                  "enum_conv: http::field::user_agent 映射缺失或错位");
    static_assert(to_field(http::field::variant_vary) == field::variant_vary,
                  "enum_conv: http::field::variant_vary 映射缺失或错位");
    static_assert(to_field(http::field::vary) == field::vary, "enum_conv: http::field::vary 映射缺失或错位");
    static_assert(to_field(http::field::vbr_info) == field::vbr_info,
                  "enum_conv: http::field::vbr_info 映射缺失或错位");
    static_assert(to_field(http::field::version) == field::version, "enum_conv: http::field::version 映射缺失或错位");
    static_assert(to_field(http::field::via) == field::via, "enum_conv: http::field::via 映射缺失或错位");
    static_assert(to_field(http::field::want_digest) == field::want_digest,
                  "enum_conv: http::field::want_digest 映射缺失或错位");
    static_assert(to_field(http::field::warning) == field::warning, "enum_conv: http::field::warning 映射缺失或错位");
    static_assert(to_field(http::field::www_authenticate) == field::www_authenticate,
                  "enum_conv: http::field::www_authenticate 映射缺失或错位");
    static_assert(to_field(http::field::x_archived_at) == field::x_archived_at,
                  "enum_conv: http::field::x_archived_at 映射缺失或错位");
    static_assert(to_field(http::field::x_device_accept) == field::x_device_accept,
                  "enum_conv: http::field::x_device_accept 映射缺失或错位");
    static_assert(to_field(http::field::x_device_accept_charset) == field::x_device_accept_charset,
                  "enum_conv: http::field::x_device_accept_charset 映射缺失或错位");
    static_assert(to_field(http::field::x_device_accept_encoding) == field::x_device_accept_encoding,
                  "enum_conv: http::field::x_device_accept_encoding 映射缺失或错位");
    static_assert(to_field(http::field::x_device_accept_language) == field::x_device_accept_language,
                  "enum_conv: http::field::x_device_accept_language 映射缺失或错位");
    static_assert(to_field(http::field::x_device_user_agent) == field::x_device_user_agent,
                  "enum_conv: http::field::x_device_user_agent 映射缺失或错位");
    static_assert(to_field(http::field::x_frame_options) == field::x_frame_options,
                  "enum_conv: http::field::x_frame_options 映射缺失或错位");
    static_assert(to_field(http::field::x_mittente) == field::x_mittente,
                  "enum_conv: http::field::x_mittente 映射缺失或错位");
    static_assert(to_field(http::field::x_pgp_sig) == field::x_pgp_sig,
                  "enum_conv: http::field::x_pgp_sig 映射缺失或错位");
    static_assert(to_field(http::field::x_ricevuta) == field::x_ricevuta,
                  "enum_conv: http::field::x_ricevuta 映射缺失或错位");
    static_assert(to_field(http::field::x_riferimento_message_id) == field::x_riferimento_message_id,
                  "enum_conv: http::field::x_riferimento_message_id 映射缺失或错位");
    static_assert(to_field(http::field::x_tiporicevuta) == field::x_tiporicevuta,
                  "enum_conv: http::field::x_tiporicevuta 映射缺失或错位");
    static_assert(to_field(http::field::x_trasporto) == field::x_trasporto,
                  "enum_conv: http::field::x_trasporto 映射缺失或错位");
    static_assert(to_field(http::field::x_verificasicurezza) == field::x_verificasicurezza,
                  "enum_conv: http::field::x_verificasicurezza 映射缺失或错位");
    static_assert(to_field(http::field::x400_content_identifier) == field::x400_content_identifier,
                  "enum_conv: http::field::x400_content_identifier 映射缺失或错位");
    static_assert(to_field(http::field::x400_content_return) == field::x400_content_return,
                  "enum_conv: http::field::x400_content_return 映射缺失或错位");
    static_assert(to_field(http::field::x400_content_type) == field::x400_content_type,
                  "enum_conv: http::field::x400_content_type 映射缺失或错位");
    static_assert(to_field(http::field::x400_mts_identifier) == field::x400_mts_identifier,
                  "enum_conv: http::field::x400_mts_identifier 映射缺失或错位");
    static_assert(to_field(http::field::x400_originator) == field::x400_originator,
                  "enum_conv: http::field::x400_originator 映射缺失或错位");
    static_assert(to_field(http::field::x400_received) == field::x400_received,
                  "enum_conv: http::field::x400_received 映射缺失或错位");
    static_assert(to_field(http::field::x400_recipients) == field::x400_recipients,
                  "enum_conv: http::field::x400_recipients 映射缺失或错位");
    static_assert(to_field(http::field::x400_trace) == field::x400_trace,
                  "enum_conv: http::field::x400_trace 映射缺失或错位");
    static_assert(to_field(http::field::xref) == field::xref, "enum_conv: http::field::xref 映射缺失或错位");

    // 枚举外的数值必须安全退回 unknown，而不是 UB 或乱值。
    static_assert(to_verb(static_cast<httplib::method>(9999)) == http::verb::unknown);
    static_assert(to_method(static_cast<http::verb>(9999)) == httplib::method::unknown);
    static_assert(to_status(static_cast<httplib::status>(9999)) == http::status::unknown);
    static_assert(to_status(static_cast<http::status>(9999)) == httplib::status::unknown);
    static_assert(to_field(static_cast<httplib::field>(9999)) == http::field::unknown);
    static_assert(to_field(static_cast<http::field>(9999)) == httplib::field::unknown);

} // namespace httplib::enum_conv
