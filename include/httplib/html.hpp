#pragma once
#include "httplib/config.hpp"
#include <boost/system/detail/error_code.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <vector>

namespace httplib
{
namespace html
{
using range_type = std::pair<int64_t, int64_t>;
using http_ranges = std::vector<range_type>;

std::time_t file_last_write_time(const fs::path& path, std::error_code& ec);

std::string format_dir_to_html(std::string_view target, const fs::path& path, boost::system::error_code ec);

// 格式化当前时间为 HTTP Date 格式
std::string format_http_current_gmt_date();
std::string format_http_gmt_date(const std::time_t& time);

std::time_t parse_http_gmt_date(const std::string& http_date);

// parser_http_ranges 用于解析 http range 请求头.
http_ranges parser_http_ranges(std::string_view range_str, size_t file_size, bool& is_valid) noexcept;

std::string generate_boundary();

} // namespace html
} // namespace httplib