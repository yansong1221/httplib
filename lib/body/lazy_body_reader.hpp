#pragma once
#include "body/any_body.hpp"
#include <algorithm>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/status.hpp>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace httplib::detail
{
    /// 按消息方向选择 header / raw / decompressed 三种 parser 类型。
    template <bool IsRequest>
    struct lazy_parser_traits
    {
        template <typename RequestParser, typename ResponseParser>
        using select = std::conditional_t<IsRequest, RequestParser, ResponseParser>;

        using header_parser = select<http::request_parser<http::empty_body>, http::response_parser<http::empty_body>>;
        using raw_parser = select<http::request_parser<http::buffer_body>, http::response_parser<http::buffer_body>>;
        using dec_parser = select<http::request_parser<body::any_body>, http::response_parser<body::any_body>>;
    };

    /** 通用 lazy body 读取器（CRTP）。

        把 `server::request::impl`（IsRequest=true）与 `client::response::impl`
        （IsRequest=false）共有的解析器状态与读取逻辑全部收敛到本模板，派生类唯一需要
        提供的差异是"数据来源"这一个钩子：

          template <typename Parser>
          net::awaitable<void> read_some(Parser& parser, boost::system::error_code& ec);

        它可以是 private，只要把本模板声明为 friend。
    */
    template <bool IsRequest, typename Derived>
    class lazy_body_reader
    {
      public:
        using traits = lazy_parser_traits<IsRequest>;
        using header_parser_t = typename traits::header_parser;
        using raw_parser_t = typename traits::raw_parser;
        using dec_parser_t = typename traits::dec_parser;

        /// 进入 lazy 状态：header 已解析完毕，保留解析器供后续流式读取。
        void
        start(std::unique_ptr<header_parser_t> header_parser, std::uint64_t body_limit)
        {
            header_parser_ = std::move(header_parser);
            body_limit_ = body_limit;
            raw_parser_.reset();
            dec_parser_.reset();
            if (header_parser_)
            {
                if constexpr (!IsRequest)
                {
                    status_ = header_parser_->get().result();
                }
                header_ = header_parser_->get().base();
            }
        }

        /// 取走 header 解析器用于整体物化（read_body）。
        std::unique_ptr<header_parser_t>
        take_header_parser()
        {
            return std::move(header_parser_);
        }

        http::fields&
        headers()
        {
            if (raw_parser_)
            {
                return raw_parser_->get().base();
            }
            if (dec_parser_)
            {
                return dec_parser_->get().base();
            }
            if (header_parser_)
            {
                return header_parser_->get().base();
            }
            return header_;
        }

        http::fields const&
        headers() const
        {
            return const_cast<lazy_body_reader*>(this)->headers();
        }

        http::status
        result() const
        {
            if constexpr (IsRequest)
            {
                return {};
            }
            else
            {
                if (raw_parser_)
                {
                    return raw_parser_->get().result();
                }
                if (dec_parser_)
                {
                    return dec_parser_->get().result();
                }
                if (header_parser_)
                {
                    return header_parser_->get().result();
                }
                return status_;
            }
        }

        bool
        is_body_done() const
        {
            if (raw_parser_)
            {
                return raw_parser_->is_done();
            }
            if (dec_parser_)
            {
                if (!dec_parser_->is_done())
                {
                    return false;
                }
                // 解析器已读完，但解压溢出数据可能还没取完。
                if (auto const* buf_body = std::get_if<body::buffer_body::value_type>(&dec_parser_->get().body()))
                {
                    return buf_body->pending.empty();
                }
                return true;
            }
            // 尚未开始读取：
            // - request：header 之后若还有 body 则未读完，直接问 header parser；
            // - response：header parser 用的是 empty_body，is_done() 会在 header 读完后
            //   立即为真（即使 body 尚未读取），因此这里必须判为未读完。
            if constexpr (IsRequest)
            {
                return !header_parser_ || header_parser_->is_done();
            }
            else
            {
                return false;
            }
        }

      protected:
        /// 流式读取原始（未解压）body：把 header_parser 转成 buffer_body 解析器。
        net::awaitable<std::size_t>
        read_some_raw_impl(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            if (!started())
            {
                ec = {};
                co_return 0;
            }
            if (!raw_parser_)
            {
                if (!header_parser_)
                {
                    ec = bad_file_descriptor();
                    co_return 0;
                }
                raw_parser_ = std::make_unique<raw_parser_t>(std::move(*header_parser_));
                raw_parser_->eager(true);
                header_parser_.reset();
            }

            for (;;)
            {
                if (raw_parser_->is_done())
                {
                    ec = {};
                    co_return 0;
                }

                auto& body = raw_parser_->get().body();
                body.data = buf.data();
                body.size = buf.size();

                co_await self().read_some(*raw_parser_, ec);
                if (ec == http::error::need_buffer)
                {
                    ec = {};
                }

                auto consumed = buf.size() - body.size;
                if (consumed > 0)
                {
                    co_return consumed;
                }
                if (ec)
                {
                    co_return 0;
                }
            }
        }

        /// 流式读取解压后的 body：把 buffer_body 放进 any_body，复用其 content-encoding 解压逻辑。
        /// 调用方缓冲写不下时溢出到 value_type::pending，下次调用先取 pending，保证不丢数据。
        net::awaitable<std::size_t>
        read_some_decompressed_impl(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            if (!started() || buf.size() == 0)
            {
                ec = {};
                co_return 0;
            }
            if (!dec_parser_)
            {
                if (!header_parser_)
                {
                    ec = bad_file_descriptor();
                    co_return 0;
                }
                dec_parser_ = std::make_unique<dec_parser_t>(std::move(*header_parser_));
                dec_parser_->eager(true);
                header_parser_.reset();
                dec_parser_->get().body().decompressed_limit = body_limit_;
                dec_parser_->get().body() = body::buffer_body::value_type {};
            }

            for (;;)
            {
                auto& buf_body = std::get<body::buffer_body::value_type>(dec_parser_->get().body());

                // 先取上一次没写完的溢出数据。
                if (!buf_body.pending.empty())
                {
                    auto n = std::min(buf.size(), buf_body.pending.size());
                    std::memcpy(buf.data(), buf_body.pending.data(), n);
                    buf_body.pending.erase(0, n);
                    ec = {};
                    co_return n;
                }

                if (dec_parser_->is_done())
                {
                    ec = {};
                    co_return 0;
                }

                buf_body.data = buf.data();
                buf_body.size = buf.size();

                co_await self().read_some(*dec_parser_, ec);
                if (ec == http::error::need_buffer)
                {
                    ec = {};
                }

                auto consumed = buf.size() - buf_body.size;
                if (consumed > 0)
                {
                    co_return consumed;
                }
                if (ec)
                {
                    co_return 0;
                }
            }
        }

      private:
        Derived&
        self()
        {
            return static_cast<Derived&>(*this);
        }

        bool
        started() const
        {
            return header_parser_ || raw_parser_ || dec_parser_;
        }

        static boost::system::error_code
        bad_file_descriptor()
        {
            return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
        }

        std::unique_ptr<header_parser_t> header_parser_;
        std::unique_ptr<raw_parser_t> raw_parser_;
        std::unique_ptr<dec_parser_t> dec_parser_;
        std::uint64_t body_limit_ = 0;
        http::fields header_;
        http::status status_ { http::status::unknown };
    };
} // namespace httplib::detail
