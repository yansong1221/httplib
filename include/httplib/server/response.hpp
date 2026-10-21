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

    /// \brief 单个 HTTP 响应。
    /// @par 线程安全
    /// 非线程安全。response 及其 writer（stream_writer / sse_writer / ndjson_writer）
    /// 只能在处理该请求的连接 strand 上访问。需要跨线程写入时，调用方必须自行
    /// post/dispatch 回该 strand，不能并发调用。socket I/O 内部会投递到连接 strand，
    /// 但这只串行化 I/O，不保护 headers / Beast message / serializer 等对象状态。
    class HTTPLIB_API response
    {
      public:
        ~response();

        // impl_ 改为 shared_ptr 后编译器会隐式生成拷贝；response 原本因 unique_ptr
        // 成员而不可拷贝，这里保持同一语义。
        response(response const&) = delete;
        response& operator=(response const&) = delete;

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

        /// 低层流式写响应：write_header 后逐段 write_body（可配合 Content-Encoding 压缩）。
        /// \note 返回共享所有权。writer 只弱持有本 response：response 销毁后（handler 已
        /// 返回、或把 writer 留给后台任务时），writer 上所有写操作返回
        /// \c net::error::operation_aborted。写操作必须在 response 存活期内完成。
        std::shared_ptr<server::stream_writer> create_stream_writer();

        class impl;

      protected:
        response(std::shared_ptr<impl> _impl);

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
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::server