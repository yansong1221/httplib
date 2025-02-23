#pragma once
#include "string.hpp"
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/http/error.hpp>
#include <filesystem>
#include <fstream>
#include <random>

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

static std::string generate_boundary()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(100000, 999999);

    return "----------------" + std::to_string(millis) + std::to_string(dist(gen));
}
static auto parse_content_disposition(std::string_view header)
{
    std::vector<std::pair<std::string_view, std::string_view>> results;

    size_t pos = 0;
    while (pos < header.size())
    {
        size_t eq = header.find('=', pos);
        if (eq == std::string_view::npos) break;

        std::string_view key = header.substr(pos, eq - pos);
        key = boost::trim_copy(key); // 去掉 key 的前后空格
        pos = eq + 1;

        std::string_view value;
        if (pos < header.size() && header[pos] == '"')
        { // 处理双引号值
            pos++;
            size_t end = pos;
            bool escape = false;
            while (end < header.size())
            {
                if (header[end] == '\\' && !escape)
                {
                    escape = true;
                }
                else if (header[end] == '"' && !escape)
                {
                    break;
                }
                else
                {
                    escape = false;
                }
                end++;
            }
            value = header.substr(pos, end - pos);
            pos = (end < header.size()) ? end + 1 : end; // 跳过 `"`
        }
        else
        { // 处理非双引号值
            size_t end = header.find(';', pos);
            if (end == std::string_view::npos) end = header.size();
            value = header.substr(pos, end - pos);
            value = boost::trim_copy(value); // 去掉 value 的前后空格
            pos = end;
        }

        results.emplace_back(key, value);

        // 处理 `; ` 分隔符
        if (pos < header.size() && header[pos] == ';') pos++;
        while (pos < header.size() && std::isspace(header[pos])) // 跳过空格
            pos++;
    }
    return results;
}
static auto split_header_field_value(std::string_view header, boost::system::error_code& ec)
{
    using namespace std::string_view_literals;

    std::vector<std::pair<std::string_view, std::string_view>> results;
    auto lines = util::split(header, "\r\n"sv);

    for (const auto& line : lines)
    {
        if (line.empty()) continue;

        auto pos = line.find(":");
        if (pos == std::string_view::npos)
        {
            ec = boost::beast::http::error::unexpected_body;
            return decltype(results) {};
        }

        auto key = boost::trim_copy(line.substr(0, pos));
        auto value = boost::trim_copy(line.substr(pos + 1));
        results.emplace_back(key, value);
    }

    return results;
}

static std::string_view buffer_to_string_view(const boost::asio::const_buffer& buffer)
{
    return std::string_view(boost::asio::buffer_cast<const char*>(buffer), boost::asio::buffer_size(buffer));
}

} // namespace util
