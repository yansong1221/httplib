#pragma once
#include <boost/nowide/convert.hpp>
#include <cmath>
#include <string>

namespace httplib {
namespace strutil {
inline static std::wstring string_to_wstring(std::string_view str) {
    return boost::nowide::widen(str);
}
inline static std::string wstring_to_string(std::wstring_view wstr) {
    return boost::nowide::narrow(wstr);
}

inline std::string to_string(float v, int width, int precision = 3) {
    char buf[20] = {0};
    std::snprintf(buf, sizeof(buf), "%*.*f", width, precision, v);
    return std::string(buf);
}

inline std::string add_suffix(float val, char const *suffix = nullptr) {
    std::string ret;

    const char *prefix[] = {"kB", "MB", "GB", "TB"};
    for (auto &i : prefix) {
        val /= 1024.f;
        if (std::fabs(val) < 1024.f) {
            ret = to_string(val, 4);
            ret += i;
            if (suffix)
                ret += suffix;
            return ret;
        }
    }
    ret = to_string(val, 4);
    ret += "PB";
    if (suffix)
        ret += suffix;
    return ret;
}
} // namespace strutil
} // namespace httplib