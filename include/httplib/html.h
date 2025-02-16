#pragma once
#include "strutil.hpp"
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <boost/url/parse.hpp>
#include <filesystem>
#include <format>

namespace httplib {
namespace html {
namespace fs = std::filesystem;

// http_ranges 用于保存 http range 请求头的解析结果.
// 例如: bytes=0-100,200-300,400-500
// 解析后的结果为: { {0, 100}, {200, 300}, {400, 500} }
// 例如: bytes=0-100,200-300,400-500,600
// 解析后的结果为: { {0, 100}, {200, 300}, {400, 500}, {600, -1} }
// 如果解析失败, 则返回空数组.
using http_ranges = std::vector<std::pair<int64_t, int64_t>>;

std::string format_dir_to_html(std::string_view target, const fs::path &path,
                               boost::system::error_code ec);

std::string fromat_error_content(int status, std::string_view reason, std::string_view server);

// 格式化当前时间为 HTTP Date 格式
std::string format_http_date();

// parser_http_ranges 用于解析 http range 请求头.
http_ranges parser_http_ranges(std::string_view range) noexcept;

} // namespace html
} // namespace httplib