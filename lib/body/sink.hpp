#pragma once
#include "body/body_state.hpp"
#include "body/multipart_parser.hpp"
#include "httplib/config.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/json/error.hpp>
#include <boost/json/monotonic_resource.hpp>
#include <boost/json/stream_parser.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace httplib::body
{
    namespace json = boost::json;

    /** 增量字节消费者（读方向的类型层）。

        逐块接收（可能已解压的）body 字节，在 `finish` 时收尾，再由 `commit` 把业务结果写进
        @ref body_state。取代旧 per-type beast reader：不再关心 Content-Encoding 与传输细节。
    */
    class sink
    {
      public:
        virtual ~sink() = default;

        /// 读取开始前配置（如 json 预分配）。content_length 可能为空（chunked）。
        virtual void
        init(std::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
        {
            boost::ignore_unused(content_length);
            ec = {};
        }

        virtual void put(net::const_buffer const& chunk, boost::system::error_code& ec) = 0;
        virtual void finish(boost::system::error_code& ec) = 0;

        /// 把结果移入 out（file / discard 等无结果时可为空操作）。
        virtual void commit(body_state& out) = 0;
    };

    using sink_ptr = std::unique_ptr<sink>;

    /// 原样累积为字符串。
    class string_sink : public sink
    {
      public:
        void
        put(net::const_buffer const& chunk, boost::system::error_code& ec) override
        {
            ec = {};
            buf_.append(static_cast<char const*>(chunk.data()), chunk.size());
        }

        void
        finish(boost::system::error_code& ec) override
        {
            ec = {};
        }

        void
        commit(body_state& out) override
        {
            out.set_string(std::move(buf_));
        }

      private:
        std::string buf_;
    };

    /// 丢弃全部字节（空 body / 无需结果时）。
    class discard_sink : public sink
    {
      public:
        void
        put(net::const_buffer const&, boost::system::error_code& ec) override
        {
            ec = {};
        }

        void
        finish(boost::system::error_code& ec) override
        {
            ec = {};
        }

        void
        commit(body_state&) override {}
    };

    /// 递增解析 JSON（`boost::json::stream_parser`）。
    class json_sink : public sink
    {
      public:
        void
        init(std::optional<std::uint64_t> const& content_length, boost::system::error_code& ec) override
        {
            if (content_length)
            {
                static constexpr std::uint64_t max_json_size = 10 * 1024 * 1024;
                auto alloc_size = (std::min)(*content_length, max_json_size);
                parser_.reset(json::make_shared_resource<json::monotonic_resource>(alloc_size));
            }
            ec = {};
        }

        void
        put(net::const_buffer const& chunk, boost::system::error_code& ec) override
        {
            ec = {};
            parser_.write_some(static_cast<char const*>(chunk.data()), chunk.size(), ec);
        }

        void
        finish(boost::system::error_code& ec) override
        {
            ec = {};
            if (parser_.done())
            {
                value_ = parser_.release();
            }
            else
            {
                ec = boost::json::error::incomplete;
            }
        }

        void
        commit(body_state& out) override
        {
            out.set_json(std::move(value_));
        }

      private:
        json::stream_parser parser_;
        boost::json::value value_;
    };

    /// 累积原始字节，收尾时按 x-www-form-urlencoded 解码。
    class query_params_sink : public sink
    {
      public:
        void
        init(std::optional<std::uint64_t> const& content_length, boost::system::error_code& ec) override
        {
            if (content_length)
            {
                buf_.reserve(static_cast<std::size_t>(*content_length));
            }
            ec = {};
        }

        void
        put(net::const_buffer const& chunk, boost::system::error_code& ec) override
        {
            ec = {};
            buf_.append(static_cast<char const*>(chunk.data()), chunk.size());
        }

        void
        finish(boost::system::error_code& ec) override
        {
            ec = {};
            if (!params_.decode(buf_))
            {
                ec = http::error::unexpected_body;
            }
        }

        void
        commit(body_state& out) override
        {
            out.set_query_params(std::move(params_));
        }

      private:
        std::string buf_;
        html::query_params params_;
    };

    /// 增量解析 multipart/form-data。
    class form_data_sink : public sink
    {
      public:
        form_data_sink(std::string content_type, html::form_data::param params = {})
            : parser_(std::move(content_type), std::move(params))
        {
        }

        void
        init(std::optional<std::uint64_t> const&, boost::system::error_code& ec) override
        {
            parser_.reset(ec);
        }

        void
        put(net::const_buffer const& chunk, boost::system::error_code& ec) override
        {
            parser_.put(chunk, ec);
        }

        void
        finish(boost::system::error_code& ec) override
        {
            parser_.finish(ec);
        }

        void
        commit(body_state& out) override
        {
            out.set_form_data(parser_.take());
        }

      private:
        multipart_parser parser_;
    };

    /// 把 body 字节写入文件（读方向的 file_body 替代）。
    class file_sink : public sink
    {
      public:
        explicit file_sink(fs::path path) : path_(std::move(path)) {}

        void
        init(std::optional<std::uint64_t> const&, boost::system::error_code& ec) override
        {
            ec = {};
            file_.open(path_, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!file_.is_open())
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::permission_denied);
            }
        }

        void
        put(net::const_buffer const& chunk, boost::system::error_code& ec) override
        {
            ec = {};
            file_.write(static_cast<char const*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            if (!file_)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::io_error);
            }
        }

        void
        finish(boost::system::error_code& ec) override
        {
            ec = {};
            if (file_.is_open())
            {
                file_.close();
            }
        }

        void
        commit(body_state&) override {}

      private:
        fs::path path_;
        std::ofstream file_;
    };

    /// 按 Content-Type 选择默认 sink（读方向的类型分发）。
    inline sink_ptr
    make_sink_for(std::string_view content_type, html::form_data::param params = {})
    {
        if (content_type.starts_with("multipart/form-data"))
        {
            return std::make_unique<form_data_sink>(std::string(content_type), std::move(params));
        }
        if (content_type.starts_with("application/json"))
        {
            return std::make_unique<json_sink>();
        }
        if (content_type.starts_with("application/x-www-form-urlencoded"))
        {
            return std::make_unique<query_params_sink>();
        }
        return std::make_unique<string_sink>();
    }

} // namespace httplib::body