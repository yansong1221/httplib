#include "httplib/url/url.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/url.hpp>
#include <cctype>
#include <format>
#include <iomanip>
#include <sstream>
#include <string>

namespace httplib::url
{

    namespace detail
    {
        static bool
        is_hex_digit(uint8_t c)
        {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }
        static std::uint8_t
        hex_to_dec(std::uint8_t c)
        {
            if (c >= '0' && c <= '9')
            {
                c -= '0';
            }
            else if (c >= 'a' && c <= 'f')
            {
                c -= 'a' - 10;
            }
            else if (c >= 'A' && c <= 'F')
            {
                c -= 'A' - 10;
            }
            return c;
        }
    } // namespace detail

    void
    url_decode(std::string& str)
    {
        size_t w = 0;
        for (size_t r = 0; r < str.size(); ++r)
        {
            uint8_t v = str[r];
            if (str[r] == '%' && r + 2 < str.size() && detail::is_hex_digit(str[r + 1])
                && detail::is_hex_digit(str[r + 2]))
            {
                v = detail::hex_to_dec(str[++r]) << 4;
                v |= detail::hex_to_dec(str[++r]);
            }
            str[w++] = v;
        }
        str.resize(w);
    }

    std::string
    url_decode(std::string_view str)
    {
        std::string decode_str(str);
        url_decode(decode_str);
        return decode_str;
    }

    std::string
    url_encode(std::string_view value)
    {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (char c : value)
        {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~')
            {
                escaped << c;
            }
            else if (c == ' ')
            {
                escaped << '+';
            }
            else
            {
                escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
            }
        }

        return escaped.str();
    }

    bool
    url_info::is_ssl() const noexcept
    {
        return boost::algorithm::iequals(scheme, "https") || boost::algorithm::iequals(scheme, "wss");
    }

    std::string
    url_info::target(bool include_fragment) const
    {
        std::string s;
        s.reserve(path.size() + query.size() + fragment.size() + 2);
        s += path;
        if (!query.empty())
        {
            s += '?';
            s += query;
        }
        if (include_fragment && !fragment.empty())
        {
            s += '#';
            s += fragment;
        }
        return s;
    }

    std::string
    url_info::to_url() const
    {
        std::string s;
        s.reserve(scheme.size() + host.size() + path.size() + query.size() + fragment.size() + 8);
        s += scheme;
        s += "://";
        if (host.find(':') != std::string::npos)
        {
            s += '[';
            s += host;
            s += ']';
        }
        else
        {
            s += host;
        }
        if (port != 0)
        {
            s += ':';
            s += std::to_string(port);
        }
        s += target();
        return s;
    }

    std::expected<url_info, boost::system::error_code>
    parse_url(std::string_view url)
    {
        auto r = boost::urls::parse_uri(url);
        if (!r)
        {
            return std::unexpected(r.error());
        }

        auto const& u = *r;
        url_info out;
        out.scheme = std::string(u.scheme());
        out.host = std::string(u.host());
        out.port = u.has_port() ? u.port_number() : 0;
        out.path = std::string(u.encoded_path());
        out.query = u.has_query() ? std::string(u.encoded_query()) : std::string {};
        out.fragment = u.has_fragment() ? std::string(u.encoded_fragment()) : std::string {};
        return out;
    }

    std::string
    make_host_value(std::string_view host_in, uint16_t port, scheme s)
    {
        std::string host(host_in);
        // IPv6字面量在 Host 头/URL 中需要方括号；已带括号的 host 不重复包裹。
        if (host.find(':') != std::string::npos && !(host.front() == '[' && host.back() == ']'))
        {
            host = std::format("[{}]", host);
        }

        if (port != default_port(s))
        {
            return std::format("{}:{}", host, port);
        }
        return host;
    }

    std::string
    make_url_value(std::string_view host, uint16_t port, scheme s, std::string_view target, std::string_view url_scheme)
    {
        using namespace std::string_view_literals;

        return std::format("{}://{}{}",
                           (url_scheme.empty() ? to_string(s) : url_scheme),
                           make_host_value(host, port, s),
                           target);
    }

} // namespace httplib::url