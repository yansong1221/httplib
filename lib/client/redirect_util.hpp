#pragma once
#include "httplib/config.hpp"
#include <boost/beast/http/fields.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::client::redirect
{

    /// Resolve a (possibly relative) redirect `Location` against the request
    /// target currently in effect, following RFC 3986 reference resolution.
    /// Always returns an absolute-path request target (with optional query).
    inline std::string
    resolve_redirect_target(std::string_view base_target, std::string_view location)
    {
        if (location.empty())
        {
            return std::string(base_target);
        }
        if (location.front() == '/')
        {
            std::string r(location);
            if (auto hash = r.find('#'); hash != std::string::npos)
            {
                r.erase(hash);
            }
            return r;
        }

        // Strip any fragment from the reference and keep the query.
        std::string ref(location);
        std::string query;
        if (auto hash = ref.find('#'); hash != std::string::npos)
        {
            ref.erase(hash);
        }
        if (auto q = ref.find('?'); q != std::string::npos)
        {
            query = ref.substr(q);
            ref.erase(q);
        }

        // Reference with an empty path (e.g. "?a=1" or "#frag"): keep the base
        // path and only replace the query.
        std::string base(base_target);
        if (auto stop = base.find_first_of("?#"); stop != std::string::npos)
        {
            base.erase(stop);
        }
        if (ref.empty())
        {
            return base + query;
        }

        // Base directory (everything up to and including the last '/').
        std::string dir = "/";
        if (auto slash = base.rfind('/'); slash != std::string::npos)
        {
            dir = base.substr(0, slash + 1);
        }

        std::string combined = dir + ref;
        bool absolute = !combined.empty() && combined.front() == '/';

        std::vector<std::string> segments;
        std::size_t i = 0;
        while (i <= combined.size())
        {
            auto next = combined.find('/', i);
            auto end = (next == std::string::npos) ? combined.size() : next;
            auto seg = combined.substr(i, end - i);
            if (seg == "..")
            {
                if (!segments.empty() && segments.back() != "..")
                {
                    segments.pop_back();
                }
            }
            else if (!seg.empty() && seg != ".")
            {
                segments.push_back(std::move(seg));
            }
            if (next == std::string::npos)
            {
                break;
            }
            i = next + 1;
        }

        std::string result = absolute ? "/" : "";
        for (std::size_t k = 0; k < segments.size(); ++k)
        {
            if (k > 0)
            {
                result.push_back('/');
            }
            result += segments[k];
        }
        if (result.empty())
        {
            result = "/";
        }
        result += query;
        return result;
    }

    /// Remove origin-bound credentials before following a cross-origin redirect,
    /// so they cannot leak to a different host.
    inline void
    strip_origin_bound_headers(http::fields& headers)
    {
        headers.erase(http::field::authorization);
        headers.erase(http::field::proxy_authorization);
        headers.erase(http::field::cookie);
        headers.erase(http::field::cookie2);
    }

} // namespace httplib::client::redirect
