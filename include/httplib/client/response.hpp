#pragma once
#include "httplib/client/read_session.hpp"
#include "httplib/config.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/html/query_params.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/json/value.hpp>
#include <boost/system/result.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::client
{

    class HTTPLIB_API response
    {
      public:
        response();
        response(response&&) noexcept;
        response& operator=(response&&) noexcept;
        ~response();

        http::status result() const;
        unsigned result_int() const;
        std::string_view operator[](http::field name) const;
        std::string_view operator[](std::string_view name) const;
        http::fields const& headers() const;
        http::fields& headers();
        http::fields const& base() const;
        http::fields& base();

        // ---- 已读完 body（eager）：同步转换 ----
        // 按内容类型取响应体（引用，不拷贝）；body 非该类型或尚未读取时抛 std::bad_variant_access
        std::string const& as_string() const;
        boost::json::value const& as_json() const;
        html::form_data const& as_form_data() const;
        html::query_params const& as_query_params() const;

        // ---- 未读完 body（lazy）：异步读取 ----
        net::awaitable<boost::system::result<std::string>> read_string();
        net::awaitable<boost::system::result<boost::json::value>> read_json();
        net::awaitable<boost::system::result<html::form_data>> read_form_data();
        net::awaitable<boost::system::result<html::query_params>> read_query_params();

        // 读取剩余 body 并物化为已读完（eager）响应
        net::awaitable<boost::system::result<response>> read_body();

        // 流式读取器（接管本响应的 body）
        std::unique_ptr<sse_reader> create_sse_reader();
        std::unique_ptr<ndjson_reader> create_ndjson_reader();

        // 流式读 body 写入文件（含 content-encoding 解压）
        net::awaitable<boost::system::error_code> read_to_file(fs::path const& save_path);

        // 低层流式读（供需要边读边处理的场景）；返回未解压的（原始）body 字节
        net::awaitable<boost::system::result<std::size_t>> read_some_raw(net::mutable_buffer const& buffer);

        // 低层流式读：返回解压后的（content-encoding 已解码）body 字节
        net::awaitable<boost::system::result<std::size_t>> read_some_decompressed(net::mutable_buffer const& buffer);
        bool is_body_done() const;

        class impl;

      private:
        response(std::shared_ptr<impl> impl);
        std::shared_ptr<impl> impl_;

        friend impl&
        get_impl(response& self)
        {
            return *self.impl_;
        }
        friend impl const&
        get_impl(response const& self)
        {
            return *self.impl_;
        }
    };

} // namespace httplib::client
