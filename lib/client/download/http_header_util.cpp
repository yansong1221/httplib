#include "http_header_util.hpp"
#include "beast_alias.hpp"
#include "enum_conv.hpp"
#include "httplib/util/misc.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <format>

namespace httplib::client::http_header_util
{
    using boost::algorithm::iequals;
    using boost::algorithm::trim;

    std::string
    fnv1a_hex(std::string_view s)
    {
        std::uint64_t h = 14695981039346656037ULL;
        for (auto c : s)
        {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ULL;
        }
        return std::format("{:016x}", h);
    }

    bool
    header_has_token(std::string_view value, std::string_view token)
    {
        for (auto part : httplib::util::split(value, ","))
        {
            if (auto eq = part.find('='); eq != std::string_view::npos)
            {
                part = boost::algorithm::trim_copy(part.substr(0, eq));
            }
            if (iequals(part, token))
            {
                return true;
            }
        }
        return false;
    }

    std::optional<std::int64_t>
    header_directive_int(std::string_view value, std::string_view name)
    {
        for (auto part : httplib::util::split(value, ","))
        {
            auto eq = part.find('=');
            if (eq == std::string_view::npos)
            {
                continue;
            }
            auto key = boost::algorithm::trim_copy(part.substr(0, eq));
            if (!iequals(key, name))
            {
                continue;
            }
            auto val = boost::algorithm::trim_copy(part.substr(eq + 1));
            try
            {
                return std::stoll(std::string(val));
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool
    response_is_encoded(httplib::headers const& headers)
    {
        auto ce = headers[field::content_encoding];
        if (ce.empty())
        {
            return false;
        }
        return !iequals(ce, "identity");
    }

    std::string
    cache_auth_scope(httplib::headers const& headers)
    {
        std::string material;
        auto append = [&](field f)
        {
            auto v = headers[f];
            if (!v.empty())
            {
                auto name = http::to_string(enum_conv::to_field(f));
                material.append(name.data(), name.size());
                material.push_back(':');
                material.append(v.data(), v.size());
                material.push_back('\n');
            }
        };
        append(field::authorization);
        append(field::proxy_authorization);
        append(field::cookie);
        append(field::cookie2);
        if (material.empty())
        {
            return {};
        }
        return fnv1a_hex(material);
    }

    bool
    response_is_cacheable(httplib::headers const& headers)
    {
        if (header_has_token(headers[field::cache_control], "no-store"))
        {
            return false;
        }
        if (header_has_token(headers[field::vary], "*"))
        {
            return false;
        }
        return true;
    }

    std::uint64_t
    parse_content_range_total(httplib::headers const& headers)
    {
        auto cr = headers[field::content_range];
        if (cr.empty())
        {
            return 0;
        }
        auto s = std::string_view(cr);
        auto slash = s.rfind('/');
        if (slash == std::string_view::npos)
        {
            return 0;
        }
        auto total_str = s.substr(slash + 1);
        if (total_str == "*")
        {
            return 0;
        }
        try
        {
            return std::stoull(std::string(total_str));
        }
        catch (...)
        {
            return 0;
        }
    }

    std::optional<std::uint64_t>
    parse_content_range_start(httplib::headers const& headers)
    {
        auto cr = headers[field::content_range];
        if (cr.empty())
        {
            return std::nullopt;
        }
        std::string_view s(cr);
        constexpr std::string_view prefix = "bytes ";
        if (!s.starts_with(prefix))
        {
            return std::nullopt;
        }
        s.remove_prefix(prefix.size());
        auto dash = s.find('-');
        if (dash == std::string_view::npos)
        {
            return std::nullopt;
        }
        try
        {
            return std::stoull(std::string(s.substr(0, dash)));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::string
    parse_content_disposition_filename(httplib::headers const& headers)
    {
        auto cd = headers[field::content_disposition];
        if (cd.empty())
        {
            return {};
        }
        std::string s(cd);
        auto idx = s.find("filename*=");
        if (idx != std::string::npos)
        {
            auto start = s.find('\'', idx + 10);
            if (start != std::string::npos)
            {
                start = s.find('\'', start + 1);
                if (start != std::string::npos)
                {
                    ++start;
                    auto end = s.find(';', start);
                    auto val = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    trim(val);
                    if (!val.empty())
                    {
                        return url::url_decode(std::string_view(val));
                    }
                }
            }
        }
        idx = s.find("filename=");
        if (idx != std::string::npos)
        {
            auto start = idx + 9;
            if (start < s.size())
            {
                if (s[start] == '"')
                {
                    ++start;
                    auto end = s.find('"', start);
                    return s.substr(start, end - start);
                }
                auto end = s.find(';', start);
                auto val = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
                trim(val);
                return val;
            }
        }
        return {};
    }

    std::optional<url::url_info>
    parse_redirect(httplib::headers const& headers)
    {
        auto loc = headers[field::location];
        if (loc.empty())
        {
            return std::nullopt;
        }
        std::string location(loc);
        if (location.starts_with("http://") || location.starts_with("https://"))
        {
            auto r = url::parse_url(location);
            if (!r)
            {
                return std::nullopt;
            }
            url::url_info t = *r;
            t.port = t.effective_port();
            t.path = r->target(false);
            t.query.clear();
            return t;
        }
        url::url_info t;
        t.path = location;
        return t;
    }

} // namespace httplib::client::http_header_util
