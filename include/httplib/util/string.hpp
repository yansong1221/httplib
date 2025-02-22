#pragma once
#include <boost/nowide/convert.hpp>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace util
{

static std::wstring string_to_wstring(std::string_view str) { return boost::nowide::widen(str); }

static std::string wstring_to_string(std::wstring_view wstr) { return boost::nowide::narrow(wstr); }

static std::string to_string(float v, int width, int precision = 3)
{
    char buf[20] = {0};
    std::snprintf(buf, sizeof(buf), "%*.*f", width, precision, v);
    return std::string(buf);
}

static std::string add_suffix(float val, char const* suffix = nullptr)
{
    std::string ret;

    const char* prefix[] = {"kB", "MB", "GB", "TB"};
    for (auto& i : prefix)
    {
        val /= 1024.f;
        if (std::fabs(val) < 1024.f)
        {
            ret = to_string(val, 4);
            ret += i;
            if (suffix) ret += suffix;
            return ret;
        }
    }
    ret = to_string(val, 4);
    ret += "PB";
    if (suffix) ret += suffix;
    return ret;
}

static std::vector<std::string_view> split(std::string_view str, std::string_view delimiter)
{ // Sanity check str
    if (str.empty()) return {};

    // Sanity check delimiter
    if (delimiter.empty()) return {str};

    // Split
    std::vector<std::string_view> parts;
    std::string_view::size_type pos = 0;
    while (pos != std::string_view::npos)
    {
        // Look for substring
        const auto pos_found = str.find(delimiter, pos);

        // Drop leading delimiters
        if (pos_found == 0)
        {
            pos += delimiter.size();
            continue;
        }

        // Capture string
        parts.emplace_back(str.substr(pos, pos_found - pos));

        // Drop trailing delimiters
        if (pos_found + delimiter.size() >= str.size()) break;

        // Move on
        if (pos_found == std::string_view::npos) break;
        pos = pos_found + delimiter.size();
    }

    return parts;
}

} // namespace util
