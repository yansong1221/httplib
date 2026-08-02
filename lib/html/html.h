#pragma once
#include "httplib/config.hpp"
#include <boost/json/value.hpp>
#include <chrono>
#include <filesystem>
#include <vector>

namespace httplib
{
    namespace html
    {

        std::time_t file_last_write_time(fs::path const& path, std::error_code& ec);

        std::string format_dir_to_html(std::string_view target, fs::path const& path, boost::system::error_code& ec);
        boost::json::value format_dir_to_json(fs::path const& path, boost::system::error_code& ec);

        // 格式化当前时间为 HTTP Date 格式
        std::string format_http_gmt_date(std::time_t const& time);

        std::time_t parse_http_gmt_date(std::string const& http_date);

        std::string generate_boundary();

    } // namespace html
} // namespace httplib