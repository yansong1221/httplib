#include "html.h"
#include "httplib/util/misc.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <random>
#include <sstream>

namespace httplib::html {

namespace detail {

inline constexpr auto head_fmt =
    R"(<html><head><meta charset="UTF-8"><title>Index of {}</title></head><body bgcolor="white"><h1>Index of {}</h1><hr><pre>)";
inline constexpr auto tail_fmt = "</pre><hr></body></html>";
inline constexpr auto body_fmt = "<a href=\"{}\">{}</a>{} {}       {}\r\n";

static void _localtime(struct tm* _Tm, const time_t* _Time)
{
#ifdef _WIN32
    localtime_s(_Tm, _Time);
#else
    localtime_r(_Time, _Tm);
#endif
}
static void _gmtime(struct tm* _Tm, const time_t* _Time)
{
#ifdef _WIN32
    gmtime_s(_Tm, _Time);
#else
    gmtime_r(_Time, _Tm);
#endif
}

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
    for (auto& i : prefix) {
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

static std::chrono::system_clock::time_point cast(const fs::file_time_type& endpoint)
{
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        endpoint - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
}

inline static long get_gmt_timezone_offset()
{
    static long offset = []() {
        std::time_t now = std::time(nullptr);
        std::tm local_tm;
        std::tm gmt_tm;
        _localtime(&local_tm, &now);
        _gmtime(&gmt_tm, &now);
        return std::mktime(&local_tm) - std::mktime(&gmt_tm);
    }();
    return offset;
}


inline static std::string make_unc_path(const fs::path& path)
{
    auto ret = path.string();

#ifdef WIN32
    if (ret.size() > MAX_PATH) {
        boost::replace_all(ret, "/", "\\");
        return "\\\\?\\" + ret;
    }
#endif

    return ret;
}
inline static std::tuple<std::string, fs::path> file_last_wirte_time(const fs::path& file)
{
    boost::system::error_code ec;
    std::string time_string;
    fs::path unc_path;

    auto ftime = fs::last_write_time(file, ec);
    if (ec) {
#ifdef WIN32
        if (file.string().size() > MAX_PATH) {
            unc_path = make_unc_path(file);
            ftime    = fs::last_write_time(unc_path, ec);
        }
#endif
    }

    if (!ec) {
        auto time = std::chrono::system_clock::to_time_t(cast(ftime));

        struct tm loc_tm;
        _localtime(&loc_tm, &time);

        char tmbuf[64] = {0};
        std::strftime(tmbuf, sizeof(tmbuf), "%m-%d-%Y %H:%M", &loc_tm);

        time_string = tmbuf;
    }

    return {time_string, unc_path};
}
inline static std::vector<std::string> format_path_list(const fs::path& path,
                                                        boost::system::error_code& ec)
{
    std::vector<std::string> path_list;
    std::vector<std::string> file_list;

    for (const auto& file_entry : fs::directory_iterator(path, ec)) {
        const auto& item = file_entry.path();

        auto [time_string, unc_path] = file_last_wirte_time(item);

        std::string rpath;

        if (fs::is_directory(unc_path.empty() ? item : unc_path, ec)) {
            auto rpath = item.filename().u8string();
            rpath += u8"/";

            int width = 50 - static_cast<int>(rpath.size());
            width     = width < 0 ? 0 : width;
            std::string space(width, ' ');
            auto show_path = rpath;
            if (show_path.size() > 50) {
                show_path = show_path.substr(0, 47);
                show_path += u8"..&gt;";
            }
            auto str = fmt::format((const char*)body_fmt,
                                   (const char*)rpath.c_str(),
                                   (const char*)show_path.c_str(),
                                   space,
                                   time_string,
                                   "-");

            path_list.push_back(str);
        }
        else {
            auto rpath = item.filename().u8string();

            int width = 50 - (int)rpath.size();
            width     = width < 0 ? 0 : width;
            std::string space(width, ' ');
            std::string filesize;
            if (unc_path.empty())
                unc_path = item;
            auto sz = static_cast<float>(fs::file_size(unc_path, ec));
            if (ec)
                sz = 0;
            filesize       = add_suffix(sz);
            auto show_path = rpath;
            if (show_path.size() > 50) {
                show_path = show_path.substr(0, 47);
                show_path += u8"..&gt;";
            }
            auto str = fmt::format((const char*)body_fmt,
                                   (const char*)rpath.c_str(),
                                   (const char*)show_path.c_str(),
                                   space,
                                   time_string,
                                   filesize);

            file_list.push_back(str);
        }
    }

    ec = {};

    std::copy(file_list.begin(), file_list.end(), std::back_inserter(path_list));
    return path_list;
}
} // namespace detail

std::string format_dir_to_html(std::string_view target,
                               const fs::path& path,
                               boost::system::error_code& ec)
{
    auto path_list = detail::format_path_list(path, ec);
    if (ec)
        return {};

    // auto target_path = detail::make_target_path(target);
    std::string head = fmt::format((const char*)detail::head_fmt, target, target);

    std::string body = fmt::format((const char*)detail::body_fmt, "../", "../", "", "", "");

    for (auto& s : path_list)
        body += s;
    body = head + body + (const char*)detail::tail_fmt;

    return body;
}
boost::json::value format_dir_to_json(const fs::path& path, boost::system::error_code& ec)
{
    boost::json::array path_list;
    for (const auto& file_entry : fs::directory_iterator(path, ec)) {
        boost::json::object obj;
        obj["last_write_time"] = file_last_write_time(path, ec);
        obj["is_dir"]          = file_entry.is_directory(ec);
        obj["filename"] = std::string((const char*)file_entry.path().filename().u8string().c_str());
        obj["filesize"] = fs::file_size(file_entry.path(), ec);
        path_list.push_back(obj);
    }
    return path_list;
}
std::time_t file_last_write_time(const fs::path& path, std::error_code& ec)
{
    auto ftime = fs::last_write_time(path, ec);
    if (ec)
        return {};

    auto sctp = detail::cast(ftime);
    return std::chrono::system_clock::to_time_t(sctp);
}

std::string format_http_gmt_date(const std::time_t& time)
{
    using namespace std::chrono;

    // Convert the time to UTC using gmtime_s (Windows) or gmtime_r (Unix-like systems)
    std::tm tm {};

    detail::_gmtime(&tm, &time);

    static const std::array<const char*, 7> WEEKDAY = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const std::array<const char*, 12> MONTH = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    char buf[64] = {0};
    std::snprintf(buf,
                  sizeof(buf),
                  "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  WEEKDAY[tm.tm_wday],
                  tm.tm_mday,
                  MONTH[tm.tm_mon],
                  tm.tm_year + 1900,
                  tm.tm_hour,
                  tm.tm_min,
                  tm.tm_sec);
    return buf;
}
std::time_t parse_http_gmt_date(const std::string& http_date)
{
    std::tm tm = {};
    std::istringstream iss(http_date);

    // Parse the HTTP date string into a tm struct
    iss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");

    if (iss.fail()) {
        throw std::runtime_error("Failed to parse HTTP date string");
    }
    // Convert tm to time_t (seconds since epoch)
    std::time_t tt = mktime(&tm); // timegm is not standard but widely available

    tt += detail::get_gmt_timezone_offset();

    // Convert time_t to system_clock::time_point
    return tt;
}

std::string generate_boundary()
{
    auto now    = std::chrono::system_clock::now().time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(100000, 999999);

    return "----------------" + std::to_string(millis) + std::to_string(dist(gen));
}


} // namespace httplib::html