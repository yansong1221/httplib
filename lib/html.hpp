#pragma once
#include "httplib/config.hpp"
#include <boost/system/detail/error_code.hpp>
#include <filesystem>
#include <format>

namespace httplib
{
namespace html
{
namespace fs = std::filesystem;

std::string format_dir_to_html(std::string_view target, const fs::path& path, boost::system::error_code ec);

std::string fromat_error_content(int status, std::string_view reason, std::string_view server);

// 格式化当前时间为 HTTP Date 格式
std::string format_http_date();

// parser_http_ranges 用于解析 http range 请求头.
http_ranges parser_http_ranges(std::string_view range_str, size_t file_size, bool& is_valid) noexcept;

} // namespace html
} // namespace httplib