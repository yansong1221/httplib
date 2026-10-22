#pragma once
#include "httplib/config.hpp"
#include "httplib/headers.hpp"
#include "httplib/url/scheme.hpp"
#include <string>
#include <string_view>
#include <utility>

namespace httplib::client::redirect
{

    struct classified_location
    {
        bool absolute = false;
        std::string value;
    };

    /// Classify a Location value and resolve RFC 3986 network-path references
    /// ("//host/path") against the current scheme. Absolute HTTP(S) URLs pass
    /// through unchanged; relative references are returned as-is.
    inline classified_location
    classify_location(std::string_view location, url::scheme current)
    {
        if (location.starts_with("//"))
        {
            std::string value;
            value.reserve(location.size() + 6);
            value.append(url::to_string(current));
            value.push_back(':');
            value.append(location);
            return { true, std::move(value) };
        }
        if (location.starts_with("http://") || location.starts_with("https://"))
        {
            return { true, std::string(location) };
        }
        return { false, std::string(location) };
    }

    /// Remove origin-bound credentials before following a cross-origin redirect,
    /// so they cannot leak to a different host.
    inline void
    strip_origin_bound_headers(httplib::headers& headers)
    {
        headers.erase(field::authorization);
        headers.erase(field::proxy_authorization);
        headers.erase(field::cookie);
        headers.erase("Cookie2");
    }

} // namespace httplib::client::redirect
