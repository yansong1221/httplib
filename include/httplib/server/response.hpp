#pragma once
#include "httplib/form_data.hpp"
#include "httplib/headers.hpp"
#include "httplib/server/server_fwd.hpp"
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace boost::json
{
    class value;
}

namespace httplib::server
{

    class HTTPLIB_API response
    {
      public:
        ~response();

        /// 全部响应头。返回的是「借用」视图：写入直接落到本响应上，拷贝它仍指向同一集合。
        /// 遍历用 all()，按名字查用 operator[]/at/count/values。
        httplib::headers base();
        httplib::headers base() const;

        std::string_view operator[](httplib::field name) const;
        std::string_view operator[](std::string_view name) const;
        std::string_view at(httplib::field name) const;
        std::string_view at(std::string_view name) const;

        void set(httplib::field name, std::string_view value);
        void set(std::string_view name, std::string_view value);

        void insert(httplib::field name, std::string_view value);
        void insert(std::string_view name, std::string_view value);

        bool has(httplib::field name) const;
        bool has(std::string_view name) const;
        void erase(httplib::field name);
        void erase(std::string_view name);
        /// 该响应头出现的次数（1 = 无重复头）。
        std::size_t count(httplib::field name) const;
        std::size_t count(std::string_view name) const;

        httplib::status result() const;
        unsigned result_int() const;

        void set_empty_content(httplib::status status);
        void set_error_content(httplib::status status);

        void set_string_content(std::string_view data,
                                std::string_view content_type,
                                httplib::status status = httplib::status::ok)
        {
            set_string_content(std::string(data), content_type, status);
        }
        void set_string_content(std::string&& data,
                                std::string_view content_type,
                                httplib::status status = httplib::status::ok);

        void set_json_content(boost::json::value const& data, httplib::status status = httplib::status::ok);
        void set_json_content(boost::json::value&& data, httplib::status status = httplib::status::ok);
        void set_file_content(fs::path const& path, httplib::headers const& req_header = {});
        void set_form_data_content(std::vector<httplib::form_data::field>&& data);

        void set_redirect(std::string_view url, httplib::status status = httplib::status::moved_permanently);

        std::unique_ptr<server::sse_writer> create_sse_writer();
        std::unique_ptr<server::ndjson_writer> create_ndjson_writer();

        // 低层流式写响应：write_header 后逐段 write_body（可配合 Content-Encoding 压缩）
        stream_writer* create_stream_writer();

        class impl;

      protected:
        response(std::unique_ptr<impl>&& _impl);

      private:
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
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server