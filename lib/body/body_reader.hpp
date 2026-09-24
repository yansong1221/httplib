#pragma once
#include "body/body_state.hpp"
#include "body/codec.hpp"
#include "body/sink.hpp"
#include "httplib/config.hpp"
#include "httplib/util/async_mutex.hpp"
#include <array>
#include <boost/asio/error.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace httplib::detail
{
    /** 通用 lazy body 读取器（非 CRTP）。

        线上固定用 Beast `http::buffer_body` 逐块读取：每次给 parser 挂上暂存缓冲，
        读到的字节喂给 `body::stream_decoder`（自写解压），解压结果再喂给 sink。业务结果
        写入 @ref body_state，是读方向的唯一入口。

        所有读取任务都收在这里：
          - 流式读取：`read_some_raw` / `read_some_decompressed`；
          - 物化：`read_body()`（按 Content-Type 自动分发 sink）或 `read_body(sink)`（强制类型）；
          - 类型化读取：`read_string` / `read_json` / `read_query_params` / `read_form_data`
            / `read_to_file`。

        方向仅由 IsRequest 决定；数据来源由构造期注入的 Source 提供：

            template <typename Parser>
            net::awaitable<void> read_some(Parser& parser, boost::system::error_code& ec);

        body 读取完成后写入本对象的 body_state；派生方如需副作用（如 client 清理 read_impl_），
        在构造时传入 on_stored 回调。
    */
    template <bool IsRequest, typename Source>
    class body_reader
    {
      public:
        using header_parser_t = http::parser<IsRequest, http::empty_body>;
        using raw_parser_t = http::parser<IsRequest, http::buffer_body>;
        using state_t = httplib::body::body_state;

        static constexpr std::size_t staging_size = 64 * 1024;

        body_reader(net::any_io_executor executor,
                    Source* source,
                    header_parser_t&& header_parser,
                    std::uint64_t body_limit,
                    std::function<void()> on_stored = {})
            : source_(source)
            , read_mutex_(std::move(executor))
            , raw_parser_(std::move(header_parser))
            , body_limit_(body_limit)
            , on_stored_(std::move(on_stored))
        {
        }
        body_reader(body_reader&&) noexcept = default;
        body_reader& operator=(body_reader&&) noexcept = default;

        /// 预设自动分发时 form_data sink 的解析参数（服务端由 router 配置注入）。
        void
        set_form_data_params(html::form_data::param params)
        {
            form_params_ = std::move(params);
        }

        /// 物化后的 body（方向无关的访问器）。
        state_t&
        state()
        {
            return state_;
        }

        state_t const&
        state() const
        {
            return state_;
        }

        bool
        is_body_done() const
        {
            if (state_.has())
            {
                return true;
            }
            if (!raw_parser_.is_done())
            {
                return false;
            }
            if (stream_decoder_)
            {
                return stream_decoder_->buffered() == 0;
            }
            return true;
        }

        /// 流式读取原始（未解压）body。
        net::awaitable<std::size_t>
        read_some_raw(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            auto lock = co_await read_mutex_.lock();
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
            auto lock = co_await read_mutex_.lock();
            if (!lock)
            {
                ec = aborted();
                co_return 0;
            }
            co_return co_await read_some_decompressed_locked(buf, ec);
        }

        /// 读剩余 body，按 Content-Type 自动分发 sink（无 body 时记为 empty）。
        net::awaitable<boost::system::error_code>
        read_body()
        {
            co_return co_await read_body_impl(nullptr);
        }

        /// 读剩余 body 进指定 sink 并物化（强制类型，即使无 body 也产出该类型）。
        net::awaitable<boost::system::error_code>
        read_body(body::sink_ptr sink)
        {
            co_return co_await read_body_impl(std::move(sink));
        }

        net::awaitable<boost::system::result<std::string>>
        read_string()
        {
            auto ec = co_await read_body(std::make_unique<body::string_sink>());
            if (ec)
            {
                co_return ec;
            }
            co_return state_.take_string();
        }

        net::awaitable<boost::system::result<boost::json::value>>
        read_json()
        {
            auto ec = co_await read_body(std::make_unique<body::json_sink>());
            if (ec)
            {
                co_return ec;
            }
            co_return state_.take_json();
        }

        net::awaitable<boost::system::result<html::query_params>>
        read_query_params()
        {
            auto ec = co_await read_body(std::make_unique<body::query_params_sink>());
            if (ec)
            {
                co_return ec;
            }
            co_return state_.take_query_params();
        }

        net::awaitable<boost::system::result<html::form_data>>
        read_form_data(html::form_data::param params = {})
        {
            std::string content_type = raw_parser_.get()[http::field::content_type];
            auto ec = co_await read_body(
                std::make_unique<body::form_data_sink>(std::move(content_type), std::move(params)));
            if (ec)
            {
                co_return ec;
            }
            co_return state_.take_form_data();
        }

        net::awaitable<boost::system::error_code>
        read_to_file(fs::path path)
        {
            co_return co_await read_body(std::make_unique<body::file_sink>(std::move(path)));
        }

      private:
        template <typename Parser>
        net::awaitable<void>
        pull(Parser& parser, boost::system::error_code& ec)
        {
            co_await source_->read_some(parser, ec);
        }

        net::awaitable<boost::system::error_code>
        read_body_impl(body::sink_ptr sink)
        {
            // body 已经物化。
            if (state_.has())
            {
                co_return boost::system::error_code {};
            }

            // 已转流式读取则拒绝：解析器已被消费，不能再用流式读一半的结果物化。
            if (stream_started_)
            {
                if (is_body_done())
                {
                    co_return boost::system::error_code {};
                }
                co_return bad_file_descriptor();
            }

            auto lock = co_await read_mutex_.lock();
            if (!lock)
            {
                co_return aborted();
            }
            // 加锁后再确认一次，避免并发 read_body 重复物化。
            if (state_.has())
            {
                co_return boost::system::error_code {};
            }
            if (stream_started_)
            {
                co_return bad_file_descriptor();
            }

            std::optional<std::uint64_t> content_length;
            if (auto len = raw_parser_.content_length())
            {
                content_length = *len;
            }
            // 无 sink（自动分发）：无 body 时记为 empty（与旧 any_body 行为一致）。
            if (!sink && raw_parser_.is_done())
            {
                state_.set_empty();
                if (on_stored_)
                {
                    on_stored_();
                }
                co_return boost::system::error_code {};
            }

            if (!sink)
            {
                auto content_type = raw_parser_.get()[http::field::content_type];
                sink = body::make_sink_for(content_type, std::move(form_params_));
            }

            boost::system::error_code ec;
            sink->init(content_length, ec);
            if (ec)
            {
                co_return ec;
            }

            // 复用流式解压读取（其内部 raw 读走协程局部缓冲，不与本输出的 out_buf 冲突）：
            // 逐块取解压结果喂给 sink，读到 0（body 已读尽）后收尾。out_buf 在协程帧内，
            // 可安全跨 co_await。
            std::array<char, staging_size> out_buf {};
            for (;;)
            {
                auto const n = co_await read_some_decompressed_locked(net::buffer(out_buf), ec);
                if (ec)
                {
                    co_return ec;
                }
                if (n == 0)
                {
                    break;
                }
                sink->put(net::buffer(out_buf.data(), n), ec);
                if (ec)
                {
                    co_return ec;
                }
            }

            sink->finish(ec);
            if (ec)
            {
                co_return ec;
            }

            body::body_state out;
            sink->commit(out);
            state_ = std::move(out);
            if (on_stored_)
            {
                on_stored_();
            }
            co_return boost::system::error_code {};
        }

        /// 流式读取原始（未解压）body：把 header_parser 转成 buffer_body 解析器。
        net::awaitable<std::size_t>
        read_some_raw_locked(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            for (;;)
            {
                if (raw_parser_.is_done())
                {
                    ec = {};
                    co_return 0;
                }
                stream_started_ = true;

                auto& body = raw_parser_.get().body();
                body.data = buf.data();
                body.size = buf.size();

                co_await pull(raw_parser_, ec);
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

        /// 流式读取解压后的 body：复用 read_some_raw_locked 逐块读原始字节到调用方 buf，
        /// feed 同步消费掉后再 drain 把解压结果写回同一 buf —— 解压多少就取走多少，
        /// 每轮解压量受调用方缓冲大小约束；内部缓冲只兜住单个 buf 装不下的产出。
        net::awaitable<std::size_t>
        read_some_decompressed_locked(net::mutable_buffer const& buf, boost::system::error_code& ec)
        {
            if (buf.size() == 0)
            {
                ec = {};
                co_return 0;
            }
            stream_started_ = true;
            if (!stream_decoder_)
            {
                std::optional<std::uint64_t> content_length;
                if (auto len = raw_parser_.content_length())
                {
                    content_length = *len;
                }
                stream_decoder_ = std::make_unique<body::stream_decoder>();
                stream_decoder_->reset(raw_parser_.get()[http::field::content_encoding],
                                       content_length,
                                       body_limit_,
                                       ec);
                if (ec)
                {
                    co_return 0;
                }
            }

            for (;;)
            {
                // 先把上一轮没取走的解压结果交给调用方。
                if (stream_decoder_->buffered() > 0)
                {
                    ec = {};
                    co_return stream_decoder_->drain(buf, ec);
                }

                // 读一块原始字节进 buf；feed 会立即把 raw 消费进内部缓冲，
                // 循环回到顶部再把解压结果 drain 写回 buf。
                auto const n = co_await read_some_raw_locked(buf, ec);
                if (ec)
                {
                    co_return 0;
                }
                if (n > 0)
                {
                    stream_decoder_->feed(net::buffer(buf.data(), n), ec);
                    if (ec)
                    {
                        co_return 0;
                    }
                }

                // 原始 body 读尽就冲刷解压器（幂等，内部 finished_ 保证只生效一次）：
                // 必须在本轮也读到数据（n>0 且 parser 恰好完成）时同样触发，否则
                // is_body_done() 会在 flush 前先返回 true，调用方丢掉 flush 出的尾部数据。
                if (!raw_parser_.is_done())
                {
                    continue;
                }
                stream_decoder_->flush(ec);
                if (ec)
                {
                    co_return 0;
                }
                if (stream_decoder_->buffered() == 0)
                {
                    ec = {};
                    co_return 0;
                }
                // 有 flush 出的残余，回到顶部 drain 交给调用方。
            }
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
        util::async_mutex read_mutex_;
        raw_parser_t raw_parser_;
        bool stream_started_ = false;

        std::unique_ptr<body::stream_decoder> stream_decoder_;
        std::uint64_t body_limit_ = 0;
        std::function<void()> on_stored_;
        html::form_data::param form_params_;
        state_t state_;
    };
} // namespace httplib::detail