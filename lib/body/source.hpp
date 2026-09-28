#pragma once
#include "html/http_ranges.hpp"
#include "httplib/config.hpp"
#include "httplib/form_data.hpp"
#include "httplib/query_params.hpp"
#include <array>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/json/serializer.hpp>
#include <boost/json/value.hpp>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "beast_alias.hpp"

namespace httplib::body
{
    namespace json = boost::json;

    /** 增量字节生产者（写方向的类型层）。

        每次 `next` 产出一块字节；`more == true` 表示后面还有块。取代旧 per-type beast writer：
        业务类型只负责产出字节，压缩与传输由外层 source 链 / `http::buffer_body` 负责。

        source 自持产出所需的业务数据（string/json/query_params/form_data 各存一份），body_writer
        只持有 source，不再另存 payload。
    */
    class source
    {
      public:
        using chunk_t = std::optional<std::pair<net::const_buffer, bool>>;

        virtual ~source() = default;
        virtual chunk_t next(boost::system::error_code& ec) = 0;

        /// 已知整体字节数时返回（供 body_writer 自动设置 Content-Length）；流式/压缩等未知时 nullopt。
        virtual std::optional<std::uint64_t>
        content_length() const
        {
            return std::nullopt;
        }
    };

    using source_ptr = std::shared_ptr<source>;

    /// 单块字符串，一次产出。
    class string_source : public source
    {
      public:
        explicit string_source(std::string data) : data_(std::move(data)) {}

        chunk_t
        next(boost::system::error_code& ec) override
        {
            ec = {};
            if (done_)
            {
                return std::nullopt;
            }
            done_ = true;
            return std::make_pair(net::buffer(data_), false);
        }

        std::optional<std::uint64_t>
        content_length() const override
        {
            return data_.size();
        }

      private:
        std::string data_;
        bool done_ = false;
    };

    /// 递增序列化 JSON（`boost::json::serializer`）。
    class json_source : public source
    {
      public:
        explicit json_source(boost::json::value value) : value_(std::move(value)) { serializer_.reset(&value_); }

        chunk_t
        next(boost::system::error_code& ec) override
        {
            ec = {};
            // serializer::read 的前置条件是 done() == false，不能再对它调用。
            if (done_)
            {
                return std::nullopt;
            }
            auto len = serializer_.read(buffer_, sizeof(buffer_));
            done_ = serializer_.done();
            if (len.size() == 0)
            {
                return std::nullopt;
            }
            return std::make_pair(net::const_buffer(len.data(), len.size()), !done_);
        }

      private:
        boost::json::value value_;
        json::serializer serializer_;
        char buffer_[32768];
        bool done_ = false;
    };

    /// 单块 urlencoded 字符串，一次产出（内部持有编码结果）。
    class query_params_source : public source
    {
      public:
        explicit query_params_source(httplib::query_params value) : buffer_(value.encoded()) {}

        chunk_t
        next(boost::system::error_code& ec) override
        {
            ec = {};
            if (done_)
            {
                return std::nullopt;
            }
            done_ = true;
            return std::make_pair(net::buffer(buffer_), false);
        }

        std::optional<std::uint64_t>
        content_length() const override
        {
            return buffer_.size();
        }

      private:
        std::string buffer_;
        bool done_ = false;
    };

    /// 无内容。
    class empty_source : public source
    {
      public:
        chunk_t
        next(boost::system::error_code& ec) override
        {
            ec = {};
            return std::nullopt;
        }

        std::optional<std::uint64_t>
        content_length() const override
        {
            return 0;
        }
    };

    /// 共享的单例空 source：无状态（不产字节、长度恒 0），set_empty() 复用同一实例，
    /// 避免每次造一个新的。
    inline source_ptr
    shared_empty_source()
    {
        static source_ptr instance = std::make_shared<empty_source>();
        return instance;
    }

    /// 递增构建 multipart/form-data（字段 + 文件流），自持 form_data。
    class form_data_source : public source
    {
      public:
        explicit form_data_source(httplib::form_data body);

        chunk_t next(boost::system::error_code& ec) override;

      private:
        httplib::form_data body_;
        std::size_t field_index_ = 0;
        beast::flat_buffer buffer_;

        std::ifstream file_stream_;
        std::uintmax_t file_remaining_ = 0;
        static constexpr std::size_t file_buf_size_ = 8192;
        std::array<char, file_buf_size_> file_buf_;

        enum class step
        {
            header,
            content,
            content_end,
            eof
        };
        step step_ = step::header;
    };

    /// 从文件（单区间 / 多区间 multipart/byteranges）流式产出字节。
    class file_source : public source
    {
      public:
        file_source(std::ifstream file, html::http_ranges ranges, std::string content_type, std::string boundary);

        std::optional<std::uint64_t>
        content_length() const override
        {
            if (!file_.is_open())
            {
                return std::nullopt;
            }
            if (ranges_.empty())
            {
                return static_cast<std::uint64_t>(file_size_);
            }
            if (ranges_.size() == 1)
            {
                auto const& range = ranges_.front();
                return static_cast<std::uint64_t>(range.second - range.first + 1);
            }
            // multipart/byteranges：帧开销未计入，交由 chunked。
            return std::nullopt;
        }

        chunk_t next(boost::system::error_code& ec) override;

      private:
        std::size_t read(char* dest, std::size_t n);

        std::ifstream file_;
        std::size_t file_size_ = 0;
        html::http_ranges ranges_;
        std::string content_type_;
        std::string boundary_;

        std::optional<int> range_index_;
        std::optional<std::uint64_t> pos_;

        enum class step
        {
            header,
            content,
            content_end,
            eof
        };
        step step_ = step::header;
        char buf_[65535] = {};
    };

} // namespace httplib::body
