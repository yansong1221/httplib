#pragma once
#include "body/body_state.hpp"
#include "body/codec.hpp"
#include "body/source.hpp"
#include "compress/compressor.hpp"
#include "httplib/config.hpp"
#include "httplib/form_data.hpp"
#include "httplib/query_params.hpp"
#include "httplib/util/async_mutex.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace httplib::detail
{
    /** 通用 lazy body 写入器（非 CRTP），与 @ref body_reader 对称。

        写方向所有操作收口在这里：
          - 内容注入：set_string / set_json / set_query_params / set_form_data / set_file
            / set_empty / reset；
          - 分帧：prepare_payload / chunked；
          - 整消息写：write_message()（source_ 驱动序列化器）；
          - 流式写：begin_stream() + write_some(data, more) / write_raw(data, more)。

        业务数据写入本对象的 @ref payload_，source_ 引用它产出字节。HTTP message 由本对象
        以成员 @ref msg_ 自持；对外只暴露 @ref base()（start-line + headers，即 Beast 的
        header_type），body 与 serializer 状态保持私有。

        所有 body 字节最终收口于两个写原语：
          - @ref write_raw_locked：无条件原样发送明文块；
          - @ref write_compressed_locked：按 message 配置的 Content-Encoding 自动压缩后再由
            raw 落地，无转换编码时退化为 raw。
        配置了转换编码时字节长度在压缩后才确定，@ref prepare_payload 自动改用 chunked 分帧。
        分帧与 raw/压缩路线的选择属调用方业务，本对象不感知 relay / chunked 等语义。

        数据落地由 attach 注入的 Task 提供（方向仅由 IsRequest 决定）：

            task->write_header(serializer, ec);   // 只写头
            task->write(serializer, ec);          // 驱动到 need_buffer/完成
    */
    template <bool IsRequest, typename Task>
    class body_writer
    {
      public:
        using message_t = http::message<IsRequest, http::buffer_body, http::fields>;
        using serializer_t = http::serializer<IsRequest, http::buffer_body, http::fields>;
        using header_type = typename message_t::header_type;
        using state_t = httplib::body::body_state;

        body_writer() = default;
        body_writer(body_writer const&) = delete;
        body_writer& operator=(body_writer const&) = delete;

        void
        merge(http::fields const& fields)
        {
            for (auto const& h : fields)
            {
                msg_.erase(h.name_string());
            }
            for (auto const& h : fields)
            {
                msg_.insert(h.name_string(), h.value());
            }
        }

        /// 发送前绑定连接写入端（client 的 request 在 send 时才拿到 http_client::impl）。
        /// 同时用连接 executor 立即构造写锁：锁必须在任何并发写之前就绪，避免惰性初始化的竞态。
        void
        attach(Task* task, net::any_io_executor ex)
        {
            task_ = task;
            write_mutex_ = std::make_unique<util::async_mutex>(std::move(ex));
        }

        // ---- header / message 元数据访问（body 保持私有） ----

        /// 只读 message（序列化状态检查等）。
        message_t const&
        message() const
        {
            return msg_;
        }

        /// 可写的 start-line + headers。
        header_type&
        base()
        {
            return msg_.base();
        }

        header_type const&
        base() const
        {
            return msg_.base();
        }

        void
        content_length(std::uint64_t value)
        {
            msg_.content_length(value);
        }

        bool
        keep_alive() const
        {
            return msg_.keep_alive();
        }

        void
        keep_alive(bool value)
        {
            msg_.keep_alive(value);
        }

        // ---- 内容注入 ----

        void
        set_string(std::string data, std::string_view content_type)
        {
            reset();
            msg_.set(http::field::content_type, content_type);
            payload_.set<std::string>(std::move(data));
            source_ = std::make_unique<body::string_source>(payload_.as<std::string>());
        }

        void
        set_json(boost::json::value data)
        {
            reset();
            msg_.set(http::field::content_type, "application/json; charset=utf-8");
            msg_.set(http::field::cache_control, "no-store");

            payload_.set<boost::json::value>(std::move(data));
            source_ = std::make_unique<body::json_source>(payload_.as<boost::json::value>());
        }

        void
        set_query_params(httplib::query_params data)
        {
            reset();
            msg_.set(http::field::content_type, "application/x-www-form-urlencoded");
            payload_.set<httplib::query_params>(std::move(data));
            source_ = std::make_unique<body::query_params_source>(payload_.as<httplib::query_params>());
        }

        void
        set_form_data(httplib::form_data data)
        {
            reset();
            msg_.set(http::field::content_type, "multipart/form-data; boundary=" + data.boundary);
            payload_.set<httplib::form_data>(std::move(data));
            source_ = std::make_unique<body::form_data_source>(payload_.as<httplib::form_data>());
        }

        /// 文件型 source（path/ranges 已由调用方封装）：只接管 source，payload 记 file 标记。
        void
        set_file(body::source_ptr source)
        {
            reset();
            payload_.set<body::file_tag>();
            source_ = std::move(source);
        }

        void
        set_empty()
        {
            reset();
            payload_.set<body::empty_tag>();
        }

        void
        reset()
        {
            reset_serializer();
            source_.reset();
            payload_.reset();
            msg_.body() = http::buffer_body::value_type {};
            encoder_.reset();
        }

        // ---- 分帧 ----

        /// 未显式设置 Content-Length 时：优先用 source 已知长度，其次空 source 记 CL:0，否则走 chunked。
        void
        prepare_payload()
        {
            // 转换编码（gzip/br/...）会改写字节长度，明文 Content-Length 不再成立，
            // 统一退化为 chunked 分帧。
            if (!msg_[http::field::content_encoding].empty() && source_)
            {
                msg_.erase(http::field::content_length);
                msg_.body() = http::buffer_body::value_type {};
                msg_.chunked(true);
                return;
            }

            if (msg_.has_content_length())
            {
                return;
            }

            std::optional<std::uint64_t> length;
            if (source_)
            {
                length = source_->content_length();
            }
            else
            {
                length = 0;
            }

            if (length)
            {
                msg_.chunked(false);
                msg_.content_length(*length);
                return;
            }
            msg_.prepare_payload();
        }

        void
        chunked(bool value)
        {
            msg_.chunked(value);
        }

        // ---- 状态访问 ----

        state_t&
        payload()
        {
            return payload_;
        }

        state_t const&
        payload() const
        {
            return payload_;
        }

        body::source*
        source()
        {
            return source_.get();
        }

        bool
        has_source() const
        {
            return source_ != nullptr;
        }

        /// 取走当前 source 的所有权（发送准备层据此叠加编码等包装）。
        body::source_ptr
        take_source()
        {
            return std::move(source_);
        }

        /// 替换当前 source。
        void
        set_source(body::source_ptr source)
        {
            source_ = std::move(source);
        }

        serializer_t&
        serializer()
        {
            if (!sr_)
            {
                sr_ = std::make_unique<serializer_t>(msg_);
            }
            return *sr_;
        }

        void
        reset_serializer()
        {
            sr_.reset();
        }

        void
        prepare_for_send()
        {
            if (sr_ && sr_->is_done())
            {
                reset_serializer();
                encoder_.reset();
            }
        }

        bool
        is_done() const
        {
            return sr_ && sr_->is_done();
        }

        /// 头部是否已写出（client 死连接重试判定用）。
        bool
        header_done() const
        {
            return sr_ && sr_->is_header_done();
        }

        // ---- 整消息写 ----

        net::awaitable<boost::system::error_code>
        write_message()
        {
            auto lock = co_await lock_write();
            if (!lock)
            {
                co_return aborted();
            }
            co_return co_await write_message_locked();
        }

        // ---- 流式写 ----

        /// 写出 header 并进入流式写模式。分帧（chunked / content-length）由调用方在
        /// @ref base() 上自行配置；本对象不感知 relay / chunked 等业务语义。
        net::awaitable<boost::system::error_code>
        begin_stream()
        {
            auto lock = co_await lock_write();
            if (!lock)
            {
                co_return aborted();
            }
            co_return co_await begin_stream_locked();
        }

        /// 流式写一块 body：按 Content-Encoding 自动压缩（无转换编码时退化为 raw）。
        net::awaitable<boost::system::error_code>
        write_some(net::const_buffer const& data, bool more)
        {
            auto lock = co_await lock_write();
            if (!lock)
            {
                co_return aborted();
            }
            co_return co_await write_some_locked(data, more);
        }

        /// 流式写一块 body：无条件原样透传（relay 等调用方指定的直通场景）。
        net::awaitable<boost::system::error_code>
        write_raw(net::const_buffer const& data, bool more)
        {
            auto lock = co_await lock_write();
            if (!lock)
            {
                co_return aborted();
            }
            if (!sr_ || !sr_->is_header_done() || sr_->is_done())
            {
                co_return invalid_argument();
            }
            co_return co_await write_raw_locked(data, more);
        }

      private:
        net::awaitable<util::async_mutex::guard>
        lock_write()
        {
            if (!write_mutex_)
            {
                co_return util::async_mutex::guard {};
            }
            co_return co_await write_mutex_->lock();
        }

        net::awaitable<boost::system::error_code>
        write_message_locked()
        {
            boost::system::error_code ec;
            if (sr_ && sr_->is_header_done())
            {
                co_return invalid_argument();
            }

            prepare_payload();
            if (!sr_)
            {
                sr_ = std::make_unique<serializer_t>(msg_);
            }

            // 逐块拉取 source，经统一原语落地；source 读尽后以空 body + more=false 收尾
            // （压缩时正是这一步冲刷编码器并写出 chunked 终止块）。
            for (;;)
            {
                body::source::chunk_t chunk;
                if (source_)
                {
                    chunk = source_->next(ec);
                    if (ec)
                    {
                        co_return ec;
                    }
                }

                if (chunk)
                {
                    ec = co_await write_compressed_locked(chunk->first, chunk->second);
                    if (ec)
                    {
                        co_return ec;
                    }
                    if (!chunk->second)
                    {
                        co_return boost::system::error_code {};
                    }
                    continue;
                }

                co_return co_await write_compressed_locked(net::const_buffer {}, false);
            }
        }

        net::awaitable<boost::system::error_code>
        begin_stream_locked()
        {
            boost::system::error_code ec;
            if (sr_ && sr_->is_header_done())
            {
                co_return invalid_argument();
            }

            // 只负责重置 body 与编码器状态；chunked / content-length 由调用方事先在 base() 上配置。
            reset();
            sr_ = std::make_unique<serializer_t>(msg_);

            auto& sr = *sr_;
            co_await task_->write_header(sr, ec);
            if (ec)
            {
                msg_.keep_alive(false);
            }
            co_return ec;
        }

        net::awaitable<boost::system::error_code>
        write_some_locked(net::const_buffer const& data, bool more)
        {
            if (!sr_ || !sr_->is_header_done() || sr_->is_done())
            {
                co_return invalid_argument();
            }
            co_return co_await write_compressed_locked(data, more);
        }

        /// 当前 message 是否配置了转换编码（gzip/br/...）；identity / 未设置时为否。
        bool
        transforms_encoding() const
        {
            auto encoding = msg_[http::field::content_encoding];
            return !encoding.empty() && compress::compressor_factory::instance().is_transform_encoding(encoding);
        }

        // ---- body 写原语（所有 body 字节最终由这两个函数落到序列化器）----

        /// 原始写：data 作为 body 字节直接交给序列化器，无条件不压缩（relay 透传 / identity）。
        net::awaitable<boost::system::error_code>
        write_raw_locked(net::const_buffer data, bool more)
        {
            msg_.body().data = data.size() > 0 ? const_cast<void*>(data.data()) : nullptr;
            msg_.body().size = data.size();
            msg_.body().more = more;

            boost::system::error_code ec;
            co_await task_->write(*sr_, ec);
            if (ec == http::error::need_buffer)
            {
                ec = {};
            }
            else if (ec)
            {
                msg_.keep_alive(false);
            }
            co_return ec;
        }

        /// 压缩写：按 message 配置的 Content-Encoding 自动压缩；非转换编码时退化为
        /// @ref write_raw_locked。data 明文喂入编码器，产物经 raw 落地；`more == false`
        /// 表示明文结束，冲刷编码器。编码器按需懒初始化。
        net::awaitable<boost::system::error_code>
        write_compressed_locked(net::const_buffer data, bool more)
        {
            if (!transforms_encoding())
            {
                co_return co_await write_raw_locked(data, more);
            }

            boost::system::error_code ec;
            if (!encoder_)
            {
                encoder_ = std::make_unique<body::stream_encoder>();
                encoder_->reset(msg_[http::field::content_encoding], ec);
                if (ec)
                {
                    msg_.keep_alive(false);
                    co_return ec;
                }
            }

            // 上一轮产物已被写尽（flush_serializer_locked 驱动到消费完），可安全清理后喂入新明文。
            encoder_->consume_all();
            encoder_->feed(data, more, ec);
            if (ec)
            {
                msg_.keep_alive(false);
                co_return ec;
            }
            if (!more)
            {
                encoder_->flush(ec);
                if (ec)
                {
                    msg_.keep_alive(false);
                    co_return ec;
                }
            }

            auto out = encoder_->buffer();
            if (out.size() == 0 && more)
            {
                // 编码器仍在缓冲（尚未产出）：本轮无数据可发，等下一次 feed。
                co_return boost::system::error_code {};
            }
            co_return co_await write_raw_locked(out, more);
        }

        static boost::system::error_code
        aborted()
        {
            return net::error::make_error_code(net::error::operation_aborted);
        }

        static boost::system::error_code
        invalid_argument()
        {
            return boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
        }

        message_t msg_;
        Task* task_ = nullptr;
        std::unique_ptr<util::async_mutex> write_mutex_;

        state_t payload_;
        body::source_ptr source_;

        std::unique_ptr<serializer_t> sr_;
        /// Content-Encoding 编码器，按需懒初始化（仅在需要压缩时存在）。
        std::unique_ptr<body::stream_encoder> encoder_;
    };
} // namespace httplib::detail
