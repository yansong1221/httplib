#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace httplib {
namespace strutil {
std::wstring string_to_wstring(std::string_view str);
std::string wstring_to_string(std::wstring_view wstr);

std::string to_string(float v, int width, int precision = 3);
std::string add_suffix(float val, char const *suffix = nullptr);

/**
     * Splits a string.
     *
     * @note Passing an empty string will result in the return container containing an empty string.
     * @note Passing an empty string as the delimiter returns { str }.
     * @note Leading and trailing delimiters will be ignored.
     *
     * @param str The string to split.
     * @param delimiter The delimiter.
     * @return The string split into the corresponding parts.
     */
std::vector<std::string_view> split(std::string_view str, std::string_view delimiter);

} // namespace strutil
} // namespace httplib