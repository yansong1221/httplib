#pragma once
#include "httplib/headers.hpp"
#include "httplib/url/url.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace httplib::client::http_header_util
{

    std::string fnv1a_hex(std::string_view s);

    bool header_has_token(std::string_view value, std::string_view token);

    std::optional<std::int64_t> header_directive_int(std::string_view value, std::string_view name);

    bool response_is_encoded(httplib::headers const& headers);

    std::string cache_auth_scope(httplib::headers const& headers);

    bool response_is_cacheable(httplib::headers const& headers);

    std::uint64_t parse_content_range_total(httplib::headers const& headers);

    std::optional<std::uint64_t> parse_content_range_start(httplib::headers const& headers);

    std::string parse_content_disposition_filename(httplib::headers const& headers);

    std::optional<url::url_info> parse_redirect(httplib::headers const& headers);

} // namespace httplib::client::http_header_util
