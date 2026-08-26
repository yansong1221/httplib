#pragma once
#include "httplib/client/read_session.hpp"
#include "httplib/config.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/html/query_params.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>
#include <boost/system/result.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::client
{

    class HTTPLIB_API lazy_response
    {
      public:
        lazy_response() = default;
        lazy_response(lazy_response&&) = default;
        lazy_response& operator=(lazy_response&&) = default;
        ~lazy_response();

        http::status result() const;
        unsigned result_int() const;
        std::string_view operator[](http::field name) const;
        std::string_view operator[](std::string_view name) const;
        http::fields const& headers() const;
        http::fields& headers();
        http::fields const& base() const;
        http::fields& base();

        // 流式读取器（接管本响应的 body）
        std::unique_ptr<sse_reader> create_sse_reader();
        std::unique_ptr<ndjson_reader> create_ndjson_reader();

        // 惰性读 body（按内容类型转换；读取失败返回错误码）
        net::awaitable<boost::system::result<std::string>> as_string();
        net::awaitable<boost::system::result<boost::json::value>> as_json();
        net::awaitable<boost::system::result<html::form_data>> as_form_data();
        net::awaitable<boost::system::result<html::query_params>> as_query_params();

        // 流式读 body 写入文件（含 content-encoding 解压）
        net::awaitable<boost::system::error_code> read_to_file(fs::path const& save_path);

        // 低层流式读（供需要边读边处理的场景）
        net::awaitable<boost::system::result<std::size_t>> read_some(net::mutable_buffer const& buffer);
        bool is_body_done() const;

        class impl;

      private:
        lazy_response(std::shared_ptr<impl> impl);
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::client
