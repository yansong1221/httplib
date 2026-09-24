#pragma once
#include "body/body_state.hpp"
#include "body/source.hpp"
#include "html/html.h"
#include "httplib/server/response.hpp"
#include "httplib/server/stream_writer.hpp"
#include "session.hpp"
#include "util/mime_types.hpp"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/version.hpp>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace httplib::server
{
    namespace detail
    {
        static std::string
        get_current_gmt_date()
        {
            static thread_local std::time_t last_time = 0;
            static thread_local std::string cache;

            auto now = time(nullptr);
            if (last_time != now)
            {
                cache = html::format_http_gmt_date(now);
                last_time = now;
            }
            return cache;
        }
    } // namespace detail

    class response::impl : public http::response<http::buffer_body>
    {
      public:
        impl(unsigned int version, bool keep_alive, std::shared_ptr<session::http_task> task) : task_(std::move(task))
        {
            this->result(http::status::not_found);
            this->version(version);
            this->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            this->set(http::field::date, detail::get_current_gmt_date());
            this->keep_alive(keep_alive);
        }

        void
        set_empty_content(http::status status)
        {
            reset_content();
            this->result(status);
            this->content_length(0);
        }

        void
        set_error_content(http::status status)
        {
            auto content = fmt::format(
                R"(<html>
<head><title>{0} {1}</title></head>
<body bgcolor="white">
<center><h1>{0} {1}</h1></center>
<hr><center>{2}</center>
</body>
</html>)",
                (int)status,
                http::obsolete_reason(status),
                this->at(http::field::server));

            this->set_string_content(std::move(content), "text/html; charset=utf-8", status);
        }

        void
        set_string_content(std::string_view data, std::string_view content_type, http::status status = http::status::ok)
        {
            set_string_content(std::string(data), content_type, status);
        }
        void
        set_string_content(std::string&& data, std::string_view content_type, http::status status = http::status::ok)
        {
            reset_content();
            this->content_length(data.size());
            this->set(http::field::content_type, content_type);
            this->result(status);
            payload_.set_string(std::move(data));
            source_ = std::make_unique<body::string_source>(payload_.as_string());
        }

        void
        set_json_content(boost::json::value const& data, http::status status = http::status::ok)
        {
            set_json_content(boost::json::value(data), status);
        }
        void
        set_json_content(boost::json::value&& data, http::status status = http::status::ok)
        {
            reset_content();
            this->result(status);
            this->set(http::field::content_type, "application/json; charset=utf-8");
            this->set(http::field::cache_control, "no-store");
            payload_.set_json(std::move(data));
            source_ = std::make_unique<body::json_source>(payload_.as_json());
        }

        void
        set_file_content(fs::path const& path, http::fields const& req_header = {})
        {
            reset_content();
            std::error_code ec;
            auto file_size = fs::file_size(path, ec);
            if (ec)
            {
                set_error_content(http::status::not_found);
                return;
            }
            auto file_write_time = html::file_last_write_time(path, ec);
            if (ec)
            {
                set_error_content(http::status::not_found);
                return;
            }

            html::http_ranges ranges;
            if (!ranges.parse(req_header[http::field::range], file_size))
            {
                this->set(http::field::content_range, fmt::format("bytes */{}", file_size));
                set_empty_content(http::status::range_not_satisfiable);
                return;
            }
            // etag
            auto file_etag_str = fmt::format("W/{}-{}", file_size, file_write_time);
            if (req_header[http::field::if_none_match] == file_etag_str)
            {
                set_empty_content(http::status::not_modified);
                return;
            }
            // last modified
            auto file_gmt_date_str = html::format_http_gmt_date(file_write_time);
            if (req_header[http::field::if_modified_since] == file_gmt_date_str)
            {
                set_empty_content(http::status::not_modified);
                return;
            }

            std::string content_type(mime::get_mime_type(path.extension().string()));
            bool const multipart = ranges.size() > 1;
            std::string boundary = multipart ? html::generate_boundary() : std::string {};

            auto file_source
                = std::make_unique<body::file_source>(path, ranges, content_type, boundary);
            if (!file_source->ok())
            {
                set_error_content(http::status::forbidden);
                return;
            }

            this->set(http::field::etag, file_etag_str);
            this->set(http::field::last_modified, file_gmt_date_str);

            if (ranges.empty())
            {
                this->set(http::field::accept_ranges, "bytes");
                this->set(http::field::content_type, content_type);
                this->result(http::status::ok);
                this->content_length(file_size);
            }
            else if (ranges.size() == 1)
            {
                auto const& range = ranges.front();
                size_t part_size = range.second + 1 - range.first;
                this->set(http::field::content_range,
                          fmt::format("bytes {}-{}/{}", range.first, range.second, file_size));
                this->set(http::field::content_type, content_type);
                this->result(http::status::partial_content);
                this->content_length(part_size);
            }
            else
            {
                this->set(http::field::content_type, fmt::format("multipart/byteranges; boundary={}", boundary));
                this->result(http::status::partial_content);
            }
            source_ = std::move(file_source);
        }

        void
        set_form_data_content(std::vector<html::form_data::field>&& data)
        {
            reset_content();
            html::form_data value;
            value.boundary = html::generate_boundary();
            value.fields = std::move(data);

            this->result(http::status::ok);
            this->set(http::field::content_type, fmt::format("multipart/form-data; boundary={}", value.boundary));
            payload_.set_form_data(std::move(value));
            source_ = std::make_unique<body::form_data_source>(payload_.as_form_data());
        }

        void
        set_redirect(std::string_view url, http::status status = http::status::moved_permanently)
        {
            this->set(http::field::location, url);
            set_empty_content(status);
        }

        void
        reset_content()
        {
            payload_.reset();
            source_.reset();
            this->body() = http::buffer_body::value_type {};
        }

        /// 按 Content-Encoding 在现有 source 上叠加编码（压缩）。
        void
        apply_encoding(std::string_view encoding)
        {
            if (!source_)
            {
                source_ = std::make_unique<body::empty_source>();
            }
            source_ = std::make_unique<body::encoded_source>(std::move(source_), encoding);
        }

        body::source*
        source()
        {
            return source_.get();
        }

        void
        set_stream_header_sent(bool sent)
        {
            stream_header_sent_ = sent;
        }
        bool
        stream_header_sent() const
        {
            return stream_header_sent_;
        }

        static response
        create(unsigned int version, bool keep_alive, std::shared_ptr<session::http_task> task)
        {
            auto _impl = std::make_unique<response::impl>(version, keep_alive, std::move(task));
            return response(std::move(_impl));
        }

        std::shared_ptr<stream_writer> stream_writer_;
        // 连接所有者（http_task）：提供写流与写超时，作用同 request 的 reader_。
        std::shared_ptr<session::http_task> task_;
        bool stream_header_sent_ = false;

        // 业务数据容器（字符串 / json / form_data），source_ 引用它产出发送字节。
        body::body_state payload_;
        body::source_ptr source_;
    };

} // namespace httplib::server
