#pragma once
#include "httplib/config.hpp"
#include "httplib/form_data.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

namespace httplib::body
{
    /** 增量 multipart/form-data 解析器。

        从旧 `form_data_body::reader` 抽出的纯状态机：逐块喂入 body 字节，产出
        `httplib::form_data`。不再绑定 Beast reader 概念与 Content-Encoding。
    */
    class multipart_parser
    {
      public:
        multipart_parser(std::string content_type, httplib::form_data::param params = {});

        /// 解析 boundary 并复位状态。
        void reset(boost::system::error_code& ec);

        void put(net::const_buffer const& chunk, boost::system::error_code& ec);
        void finish(boost::system::error_code& ec);

        httplib::form_data&
        result()
        {
            return body_;
        }

        httplib::form_data
        take()
        {
            return std::move(body_);
        }

      private:
        void write_content(std::string_view data, boost::system::error_code& ec);

        httplib::form_data body_;
        std::string content_type_;

        std::string boundary_;
        std::string boundary_line_;
        std::string boundary_line_last_;
        std::string delim_field_;
        std::string delim_final_;

        enum class step
        {
            boundary_line,
            boundary_header,
            boundary_content,
            eof
        };
        step step_ = step::boundary_line;
        httplib::form_data::field field_data_;

        // 可能是 boundary 分隔符前半段的字节，保留到下一块再判定。
        std::string pending_;
        std::string combined_;

        std::ofstream file_stream_;
        fs::path current_file_path_;
        std::uint64_t file_bytes_written_ = 0;
    };
} // namespace httplib::body
