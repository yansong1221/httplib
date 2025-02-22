#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <filesystem>
#include <fstream>

namespace util
{
using boost::asio::buffer;
using boost::beast::bind_front_handler;
using boost::beast::buffers_to_string;

/**
 * Returns an std::string which represents the raw bytes of the file.
 *
 * @param path The path to the file.
 * @return The content of the file as it resides on the disk - byte by byte.
 */
[[nodiscard]] static inline std::string file_contents(const std::filesystem::path& path)
{
    // Sanity check
    if (!std::filesystem::is_regular_file(path)) return {};

    // Open the file
    // Note that we have to use binary mode as we want to return a string
    // representing matching the bytes of the file on the file system.
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) return {};

    // Read contents
    std::string content {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

    // Close the file
    file.close();

    return content;
}

/**
 * Convert a hex value to a decimal value.
 *
 * @param c The hexadecimal input.
 * @return The decimal output.
 */
[[nodiscard]] static inline std::uint8_t hex2dec(std::uint8_t c)
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
 * @note As the replaced characters are "shorter" than the original input we can perform the
 *       replacement in-place as long as we're somewhat careful not to fuck up.
 *
 * @param str The string to decode.
 */
// ToDo: Consider using Boost.URL instead
static inline void url_decode(std::string& str)
{
    size_t w = 0;
    for (size_t r = 0; r < str.size(); ++r)
    {
        uint8_t v = str[r];
        if (str[r] == '%')
        {
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
static std::string_view buffer_to_string_view(const boost::asio::const_buffer& buffer)
{
    return std::string_view(boost::asio::buffer_cast<const char*>(buffer), boost::asio::buffer_size(buffer));
}

} // namespace util
