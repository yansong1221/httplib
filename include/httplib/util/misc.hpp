#pragma once
#include "httplib/config.hpp"
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace httplib::util {
/**
 * Convert a hex value to a decimal value.
 *
 * @param c The hexadecimal input.
 * @return The decimal output.
 */
static inline std::uint8_t hex2dec(std::uint8_t c)
{
    if (c >= '0' && c <= '9')
        c -= '0';

    else if (c >= 'a' && c <= 'f')
        c -= 'a' - 10;

    else if (c >= 'A' && c <= 'F')
        c -= 'A' - 10;

    return c;
}

/**
 * Decodes an URL.
 *
 * @details This function replaces %<hex> with the corresponding characters.
 *          See https://en.wikipedia.org/wiki/Percent-encoding
 *
 * @note As the replaced characters are "shorter" than the original input we can perform
 * the replacement in-place as long as we're somewhat careful not to fuck up.
 *
 * @param str The string to decode.
 */
// ToDo: Consider using Boost.URL instead
static inline void url_decode(std::string& str)
{
    size_t w = 0;
    for (size_t r = 0; r < str.size(); ++r) {
        uint8_t v = str[r];
        if (str[r] == '%') {
            v = hex2dec(str[++r]) << 4;
            v |= hex2dec(str[++r]);
        }
        str[w++] = v;
    }
    str.resize(w);
}
static inline std::string url_decode(std::string_view str)
{
    std::string decode_str(str);
    url_decode(decode_str);
    return decode_str;
}
static inline std::string url_encode(std::string_view value)
{
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        }
        else if (c == ' ') {
            escaped << '+';
        }
        else {
            escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
        }
    }

    return escaped.str();
}
static std::vector<std::string_view> split(std::string_view str, std::string_view delimiter)
{ // Sanity check str
    if (str.empty())
        return {};

    // Sanity check delimiter
    if (delimiter.empty())
        return {str};

    // Split
    std::vector<std::string_view> parts;
    std::string_view::size_type pos = 0;
    while (pos != std::string_view::npos) {
        // Look for substring
        const auto pos_found = str.find(delimiter, pos);

        // Drop leading delimiters
        if (pos_found == 0) {
            pos += delimiter.size();
            continue;
        }

        // Capture string
        parts.emplace_back(boost::trim_copy(str.substr(pos, pos_found - pos)));

        // Drop trailing delimiters
        if (pos_found + delimiter.size() >= str.size())
            break;

        // Move on
        if (pos_found == std::string_view::npos)
            break;
        pos = pos_found + delimiter.size();
    }

    return parts;
}

static std::string_view buffer_to_string_view(const boost::asio::const_buffer& buffer)
{
    return std::string_view(static_cast<const char*>(buffer.data()), buffer.size());
}


} // namespace httplib::util
