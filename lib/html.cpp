#pragma once
#include "httplib/html.hpp"

#include "httplib/util/misc.hpp"
#include "httplib/util/string.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <boost/url/parse.hpp>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <random>
#include <sstream>

namespace httplib::html
{

namespace detail
{

inline constexpr auto head_fmt =
    R"(<html><head><meta charset="UTF-8"><title>Index of {}</title></head><body bgcolor="white"><h1>Index of {}</h1><hr><pre>)";
inline constexpr auto tail_fmt = "</pre><hr></body></html>";
inline constexpr auto body_fmt = "<a href=\"{}\">{}</a>{} {}       {}\r\n";

inline static long get_gmt_timezone_offset()
{
    static long offset = []()
    {
        std::time_t now = std::time(nullptr);
        std::tm local_tm;
        std::tm gmt_tm;
#ifdef _WIN32
        localtime_s(&local_tm, &now);
        gmtime_s(&gmt_tm, &now);
#else
        localtime_r(&now, &local_tm);
        gmtime_r(&now, &gmt_tm);
#endif
        return std::mktime(&local_tm) - std::mktime(&gmt_tm);
    }();
    return offset;
}

inline static std::string make_unc_path(const fs::path& path)
{
    auto ret = path.string();

#ifdef WIN32
    if (ret.size() > MAX_PATH)
    {
        boost::replace_all(ret, "/", "\\");
        return "\\\\?\\" + ret;
    }
#endif

    return ret;
}
inline static std::tuple<std::string, fs::path> file_last_wirte_time(const fs::path& file)
{
    static auto loc_time = [](auto t) -> struct tm*
    {
        using time_type = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<time_type, std::filesystem::file_time_type>)
        {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            auto time = std::chrono::system_clock::to_time_t(sctp);
            return std::localtime(&time);
        }
        else if constexpr (std::is_same_v<time_type, std::time_t>)
        {
            return std::localtime(&t);
        }
        else
        {
            static_assert(!std::is_same_v<time_type, time_type>, "time type required!");
        }
    };

    boost::system::error_code ec;
    std::string time_string;
    fs::path unc_path;

    auto ftime = fs::last_write_time(file, ec);
    if (ec)
    {
#ifdef WIN32
        if (file.string().size() > MAX_PATH)
        {
            unc_path = make_unc_path(file);
            ftime = fs::last_write_time(unc_path, ec);
        }
#endif
    }

    if (!ec)
    {
        auto tm = loc_time(ftime);

        char tmbuf[64] = {0};
        std::strftime(tmbuf, sizeof(tmbuf), "%m-%d-%Y %H:%M", tm);

        time_string = tmbuf;
    }

    return {time_string, unc_path};
}
inline static std::vector<std::string> format_path_list(const fs::path& path, boost::system::error_code& ec)
{
    fs::directory_iterator end;
    fs::directory_iterator it(path, ec);
    if (ec)
    {
        return {};
    }

    std::vector<std::string> path_list;
    std::vector<std::string> file_list;

    for (; it != end; it++)
    {
        const auto& item = it->path();

        auto [time_string, unc_path] = file_last_wirte_time(item);

        std::string rpath;

        if (fs::is_directory(unc_path.empty() ? item : unc_path, ec))
        {
            auto rpath = util::wstring_to_string(item.filename().wstring());
            rpath += "/";

            int width = 50 - static_cast<int>(rpath.size());
            width = width < 0 ? 0 : width;
            std::string space(width, ' ');
            auto show_path = rpath;
            if (show_path.size() > 50)
            {
                show_path = show_path.substr(0, 47);
                show_path += "..&gt;";
            }
            auto str = fmt::format(body_fmt, rpath, show_path, space, time_string, "-");

            path_list.push_back(str);
        }
        else
        {
            auto rpath = util::wstring_to_string(item.filename().wstring());

            int width = 50 - (int)rpath.size();
            width = width < 0 ? 0 : width;
            std::string space(width, ' ');
            std::string filesize;
            if (unc_path.empty()) unc_path = item;
            auto sz = static_cast<float>(fs::file_size(unc_path, ec));
            if (ec) sz = 0;
            filesize = util::add_suffix(sz);
            auto show_path = rpath;
            if (show_path.size() > 50)
            {
                show_path = show_path.substr(0, 47);
                show_path += "..&gt;";
            }
            auto str = fmt::format(body_fmt, rpath, show_path, space, time_string, filesize);

            file_list.push_back(str);
        }
    }

    ec = {};

    path_list.insert(path_list.end(), file_list.begin(), file_list.end());

    return path_list;
}
inline static std::string make_target_path(std::string_view target)
{
    std::string url = "http://example.com";
    if (target.starts_with("/"))
        url += target;
    else
    {
        url += "/";
        url += target;
    }

    auto result = boost::urls::parse_uri(url);
    if (result.has_error()) return std::string(target);

    return result->path();
}
} // namespace detail

std::string format_dir_to_html(std::string_view target, const fs::path& path, boost::system::error_code ec)
{
    auto path_list = detail::format_path_list(path, ec);
    if (ec) return {};

    auto target_path = detail::make_target_path(target);
    std::string head = fmt::format(detail::head_fmt, target_path, target_path);

    std::string body = fmt::format(detail::body_fmt, "../", "../", "", "", "");

    for (auto& s : path_list)
        body += s;
    body = head + body + detail::tail_fmt;

    return body;
}
std::time_t file_last_write_time(const fs::path& path, std::error_code& ec)
{
    auto ftime = fs::last_write_time(path, ec);
    if (ec) return {};

    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sctp);
}

std::string format_http_current_gmt_date() { return format_http_gmt_date(time(nullptr)); }

std::string format_http_gmt_date(const std::time_t& time)
{
    using namespace std::chrono;

    // Convert the time to UTC using gmtime_s (Windows) or gmtime_r (Unix-like systems)
    std::tm tm {};
#ifdef _WIN32
    gmtime_s(&tm, &time); // Thread-safe for Windows
#else
    gmtime_r(&time, &tm); // Thread-safe for Unix-like systems
#endif

    // Format the date in HTTP format
    std::ostringstream oss;
    oss << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");

    return oss.str();
}
std::time_t parse_http_gmt_date(const std::string& http_date)
{
    std::tm tm = {};
    std::istringstream iss(http_date);

    // Parse the HTTP date string into a tm struct
    iss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");

    if (iss.fail())
    {
        throw std::runtime_error("Failed to parse HTTP date string");
    }
    // Convert tm to time_t (seconds since epoch)
    std::time_t tt = mktime(&tm); // timegm is not standard but widely available

    tt += detail::get_gmt_timezone_offset();

    // Convert time_t to system_clock::time_point
    return tt;
}

http_ranges parser_http_ranges(std::string_view range_str, size_t file_size, bool& is_valid) noexcept
{
    is_valid = true;
    range_str = boost::trim_copy(range_str);
    if (range_str.empty()) return {};
    if (!range_str.starts_with("bytes="))
    {
        is_valid = false;
        return {};
    }
    range_str.remove_prefix(6);

    if (range_str.find("--") != std::string_view::npos)
    {
        is_valid = false;
        return {};
    }

    if (range_str == "-")
    {
        return {{0, file_size - 1}};
    }

    http_ranges vec;
    auto ranges = util::split(range_str, ",");
    for (const auto& range : ranges)
    {
        auto sub_range = util::split(range, "-");
        auto fist_range = boost::trim_copy(sub_range[0]);

        int start = 0;
        if (fist_range.empty())
        {
            start = -1;
        }
        else
        {
            auto [ptr, ec] = std::from_chars(fist_range.data(), fist_range.data() + fist_range.size(), start);
            if (ec != std::errc {})
            {
                is_valid = false;
                return {};
            }
        }

        int end = 0;
        if (sub_range.size() == 1)
        {
            end = file_size - 1;
        }
        else
        {
            auto second_range = boost::trim_copy(sub_range[1]);
            if (second_range.empty())
            {
                end = file_size - 1;
            }
            else
            {
                auto [ptr, ec] = std::from_chars(second_range.data(), second_range.data() + second_range.size(), end);
                if (ec != std::errc {})
                {
                    is_valid = false;
                    return {};
                }
            }
        }

        if (start > 0 && (start >= file_size || start == end))
        {
            // out of range
            is_valid = false;
            return {};
        }

        if (end > 0 && end >= file_size)
        {
            end = file_size - 1;
        }

        if (start == -1)
        {
            start = file_size - end;
            end = file_size - 1;
        }

        vec.push_back({start, end});
    }
    return vec;
}

std::string generate_boundary()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(100000, 999999);

    return "----------------" + std::to_string(millis) + std::to_string(dist(gen));
}

query_params parse_http_query_params(std::string_view content, bool& is_valid)
{
    is_valid = true;
    if (content.empty()) return {};

    query_params result;
    for (const auto& item : util::split(content, "&"))
    {
        auto key_val = util::split(item, "=");

        if (key_val.size() != 2)
        {
            is_valid = false;
            return {};
        }
        auto key = util::url_decode(key_val[0]);
        auto val = util::url_decode(key_val[1]);

        result.emplace(key, val);
    }
    return result;
}

std::string make_http_query_params(const query_params& params)
{
    std::vector<std::string> tokens;
    for (const auto& item : params)
    {
        auto token = util::url_encode(item.first);
        token += "=";
        token += util::url_encode(item.second);

        tokens.push_back(token);
    }
    return boost::join(tokens, "&");
}

} // namespace httplib::html