#include "httplib/util/misc.hpp"
#include <boost/algorithm/string/trim.hpp>
#include <fmt/format.h>
#include <iomanip>
#include <sstream>

namespace httplib::util
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
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'
                || c == '~')
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

    std::vector<std::string_view>
    split(std::string_view str, std::string_view delimiter, bool compress)
    {
        // Sanity check str
        if (str.empty())
        {
            return {};
        }

        // Sanity check delimiter
        if (delimiter.empty())
        {
            return { str };
        }

        // Split
        std::vector<std::string_view> parts;
        std::string_view::size_type pos = 0;
        while (pos != std::string_view::npos)
        {
            // Look for substring
            auto const pos_found = str.find(delimiter, pos);

            // Drop leading delimiters
            if (pos_found == 0)
            {
                pos += delimiter.size();
                continue;
            }

            auto s = str.substr(pos, pos_found - pos);
            if (compress)
            {
                s = boost::trim_copy(s);
            }
            // Capture string
            parts.emplace_back(s);

            // Drop trailing delimiters
            if (pos_found + delimiter.size() >= str.size())
            {
                break;
            }

            // Move on
            if (pos_found == std::string_view::npos)
            {
                break;
            }
            pos = pos_found + delimiter.size();
        }

        return parts;
    }

    std::string_view
    buffer_to_string_view(boost::asio::const_buffer const& buffer)
    {
        return std::string_view(static_cast<char const*>(buffer.data()), buffer.size());
    }

    std::string
    make_host_value(std::string_view host, uint16_t port, bool ssl)
    {
        if ((ssl && port != 443) || (!ssl && port != 80))
        {
            return fmt::format("{}:{}", host, port);
        }
        return std::string(host);
    }

    std::string
    make_url_value(std::string_view host, uint16_t port, bool ssl, std::string_view target, std::string_view scheme)
    {
        using namespace std::string_view_literals;

        return std::format("{}://{}{}",
                           (scheme.empty() ? (ssl ? "https" : "http") : scheme),
                           make_host_value(host, port, ssl),
                           target);
    }

} // namespace httplib::util
