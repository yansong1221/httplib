#pragma once
#include "body/body_state.hpp"
#include "httplib/config.hpp"
#include "httplib/util/async_mutex.hpp"
#include <boost/asio/error.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

namespace httplib::detail
{
    /** 通用 lazy body 读取器（非 CRTP）。

        把 inbound 消息（server::request / client::response）共有的解析器状态、读取串行化
        （read_mutex_）与读取逻辑全部收敛到本模板。方向仅由 IsRequest 决定；数据来源由构造期
        注入的 Source 提供：

            // 底层读一次（Source 需提供）
            template <typename Parser>
            net::awaitable<void> read_some(Parser& parser, boost::system::error_code& ec);

        body 读取完成后写入本对象的 body_state；派生方如需副作用（如 client 清理
        read_impl_），在 start() 时传入 on_stored 回调。
    */
    template <bool IsRequest, typename Source>
    class body_reader
    {
      public:
        using header_parser_t = http::parser<IsRequest, http::empty_body>;
        using raw_parser_t = http::parser<IsRequest, http::buffer_body>;
        using any_parser_t = http::parser<IsRequest, body::any_body>;
        using message_t = http::message<IsRequest, body::any_body>;
        using body_setup_fn = std::function<void(message_t&)>;
        using body_state = httplib::body::body_state;

        body_reader() = default;
        body_reader(body_reader&&) noexcept = default;
        body_reader& operator=(body_reader&&) noexcept = default;

        /// 进入 lazy 状态：header 已解析完毕，保留解析器供后续流式读取。
        void
        start(Source* source,
              std::unique_ptr<header_parser_t> header_parser,
              std::uint64_t body_limit,
              net::any_io_executor executor,
              std::function<void()> on_stored = {})
        {
            source_ = source;
            read_mutex_ = std::make_unique<util::async_mutex>(std::move(executor));
            header_parser_ = std::move(header_parser);
            body_limit_ = body_limit;
            on_stored_ = std::move(on_stored);
            raw_parser_.reset();
            any_parser_.reset();
        }

        /// 物化后的 body（方向无关的访问器）。
        body_state&
        state()
        {
            return state_;
        }

        body_state const&
        state() const
        {
            return state_;
        }

        bool
        is_body_done() const
        {
            if (state_.ready())
            {
                return true;
            }
            if (raw_parser_)
            {
                return raw_parser_->is_done();
            }
            if (any_parser_)
            {
                if (!any_parser_->is_done())
                {
                    return false;
                }
                // 解析器已读完，但解压溢出数据可能还没取完。
                if (auto const* buf_body = std::get_if<body::buffer_body::value_type>(&any_parser_->get().body()))
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

        /// 流式读取原始（未解压）body。
        net::awaitable<std::size_t>
        read_some_raw(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            if (!started())
            {
                ec = {};
                co_return 0;
            }
            auto lock = co_await read_mutex_->lock();
            if (!lock)
            {
                ec = aborted();
                co_return 0;
            }
            co_return co_await read_some_raw_locked(buf, ec);
        }

        /// 流式读取解压后的 body。
        net::awaitable<std::size_t>
        read_some_decompressed(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            if (!started())
            {
                ec = {};
                co_return 0;
            }
            auto lock = co_await read_mutex_->lock();
            if (!lock)
            {
                ec = aborted();
                co_return 0;
            }
            co_return co_await read_some_decompressed_locked(buf, ec);
        }

        /// 读取剩余 body 并按 body_setup 物化到本消息。
        net::awaitable<void>
        read_body(body_setup_fn const& body_setup, boost::system::error_code& ec)
        {
            // body 已经物化（server 非 lazy / client eager）或尚未进入 lazy 状态。
            if (state_.ready() || !started())
            {
                ec = {};
                co_return;
            }

            auto lock = co_await read_mutex_->lock();
            if (!lock)
            {
                ec = aborted();
                co_return;
            }
            // 加锁后再确认一次，避免并发 read_body 重复物化。
            if (state_.ready() || !started())
            {
                ec = {};
                co_return;
            }

            auto header_parser = take_header_parser();
            if (!header_parser)
            {
                // 已物化完成则视为成功；已转流式读取则拒绝。
                if (is_body_done())
                {
                    ec = {};
                    co_return;
                }
                ec = bad_file_descriptor();
                co_return;
            }

            any_parser_t body_parser(std::move(*header_parser));
            body_parser.eager(true);
            if (body_setup)
            {
                body_setup(body_parser.get());
            }
            body_parser.get().body().decompressed_limit = body_limit_;

            while (!body_parser.is_done())
            {
                co_await pull(body_parser, ec);
                if (ec)
                {
                    co_return;
                }
            }
            store_body(body_parser.release());
            ec = {};
        }

      private:
        template <typename Parser>
        net::awaitable<void>
        pull(Parser& parser, boost::system::error_code& ec)
        {
            co_await source_->read_some(parser, ec);
        }

        /// 取走 header 解析器用于整体物化（read_body）。
        std::unique_ptr<header_parser_t>
        take_header_parser()
        {
            return std::move(header_parser_);
        }

        void
        store_body(message_t&& msg)
        {
            state_.assign(std::move(msg.body()));
            if (on_stored_)
            {
                on_stored_();
            }
        }

        /// 流式读取原始（未解压）body：把 header_parser 转成 buffer_body 解析器。
        net::awaitable<std::size_t>
        read_some_raw_locked(net::mutable_buffer const& buf, boost::system::error_code& ec)
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
                raw_parser_->eager(false);
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

                co_await pull(*raw_parser_, ec);
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
        read_some_decompressed_locked(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            if (!started() || buf.size() == 0)
            {
                ec = {};
                co_return 0;
            }
            if (!any_parser_)
            {
                if (!header_parser_)
                {
                    ec = bad_file_descriptor();
                    co_return 0;
                }
                any_parser_ = std::make_unique<any_parser_t>(std::move(*header_parser_));
                any_parser_->eager(false);
                header_parser_.reset();
                any_parser_->get().body().decompressed_limit = body_limit_;
                any_parser_->get().body() = body::buffer_body::value_type {};
            }

            for (;;)
            {
                auto& buf_body = std::get<body::buffer_body::value_type>(any_parser_->get().body());

                // 先取上一次没写完的溢出数据。
                if (!buf_body.pending.empty())
                {
                    auto n = std::min(buf.size(), buf_body.pending.size());
                    std::memcpy(buf.data(), buf_body.pending.data(), n);
                    buf_body.pending.erase(0, n);
                    ec = {};
                    co_return n;
                }

                if (any_parser_->is_done())
                {
                    ec = {};
                    co_return 0;
                }

                buf_body.data = buf.data();
                buf_body.size = buf.size();

                co_await pull(*any_parser_, ec);
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

        bool
        started() const
        {
            return header_parser_ || raw_parser_ || any_parser_;
        }

        static boost::system::error_code
        aborted()
        {
            return net::error::make_error_code(net::error::operation_aborted);
        }

        static boost::system::error_code
        bad_file_descriptor()
        {
            return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
        }

        Source* source_ = nullptr;
        std::unique_ptr<util::async_mutex> read_mutex_;
        std::unique_ptr<header_parser_t> header_parser_;
        std::unique_ptr<raw_parser_t> raw_parser_;
        std::unique_ptr<any_parser_t> any_parser_;
        std::uint64_t body_limit_ = 0;
        std::function<void()> on_stored_;
        body_state state_;
    };
} // namespace httplib::detail
