#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/client/read_session.hpp"
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>
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

        // 惰性读 body（一次性，数据归调用方）
        net::awaitable<body::any_body::value_type> read_body();
        net::awaitable<std::string> read_text();
        net::awaitable<boost::json::value> read_json();

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
