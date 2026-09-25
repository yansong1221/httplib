#pragma once
#include "body/body_state.hpp"
#include "body/codec.hpp"
#include "body/source.hpp"
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
          - 编码与分帧：apply_encoding / prepare_payload / chunked；
          - 整消息写：write_message()（source_ 驱动序列化器）；
          - 流式写：begin_stream(mode) + write_some(data, more)。

        业务数据写入本对象的 @ref payload_，source_ 引用它产出字节。Beast 的
        `http::buffer_body::writer` 只接受 const message，写循环要修改的是调用方持有的
        message body（scratch），故本对象持有 message 引用 @p msg，而不是像 body_reader
        那样自持 parser。

        数据落地由构造期/attach 注入的 Task 提供（方向仅由 IsRequest 决定）：

            task->write_header(serializer, ec);   // 只写头
            task->write_some(serializer, ec);     // 单步写（整消息驱动用）
            task->write(serializer, ec);          // 写到 need_buffer/完成（流式块用）
    */
    template <bool IsRequest, typename Task>
    class body_writer
    {
      public:
        using message_t = http::message<IsRequest, http::buffer_body, http::fields>;
        using serializer_t = http::serializer<IsRequest, http::buffer_body, http::fields>;
        using state_t = httplib::body::body_state;

        enum class stream_mode
        {
            /// 透传原始字节，不做二次压缩（反向代理）。
            relay,
            /// 自身流式输出（chunked，可按 Content-Encoding 压缩）。
            chunked,
        };

        explicit body_writer(message_t& msg, Task* task = nullptr, net::any_io_executor ex = {})
            : msg_(msg)
            , task_(task)
            , executor_(std::move(ex))
        {
        }

        body_writer(body_writer const&) = delete;
        body_writer& operator=(body_writer const&) = delete;

        /// 发送前绑定连接写入端（client 的 request 在 send 时才拿到 http_client::impl）。
        void
        attach(Task* task, net::any_io_executor ex)
        {
            task_ = task;
            executor_ = std::move(ex);
        }

        // ---- 内容注入 ----

        void
        set_string(std::string data, std::string_view content_type)
        {
            reset();
            msg_.content_length(data.size());
            msg_.set(http::field::content_type, content_type);
            payload_.set<std::string>(std::move(data));
            source_ = std::make_unique<body::string_source>(payload_.as<std::string>());
        }

        void
        set_json(boost::json::value data, std::string_view content_type, bool no_store = false)
        {
            reset();
            msg_.set(http::field::content_type, content_type);
            if (no_store)
            {
                msg_.set(http::field::cache_control, "no-store");
            }
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
            payload_.reset();
            source_.reset();
            msg_.body() = http::buffer_body::value_type {};
        }

        // ---- 编码与分帧 ----

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

        /// 未显式设置 Content-Length 时，无 source 记 CL:0，否则走 chunked。
        void
        prepare_payload()
        {
            if (msg_.has_content_length())
            {
                return;
            }
            if (!source_)
            {
                msg_.content_length(0);
            }
            else
            {
                msg_.prepare_payload();
            }
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

        bool
        header_sent() const
        {
            return header_sent_;
        }

        bool
        is_done() const
        {
            return sr_ && sr_->is_done();
        }

        /// 上次 write_message 返回时头部是否已写出（client 死连接重试判定用）。
        bool
        header_done() const
        {
            return last_header_done_;
        }

        void
        reset_serializer()
        {
            sr_.reset();
            header_sent_ = false;
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

        net::awaitable<boost::system::error_code>
        begin_stream(stream_mode mode)
        {
            auto lock = co_await lock_write();
            if (!lock)
            {
                co_return aborted();
            }
            co_return co_await begin_stream_locked(mode);
        }

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

      private:
        net::awaitable<util::async_mutex::guard>
        lock_write()
        {
            if (!write_mutex_)
            {
                write_mutex_.emplace(executor_);
            }
            co_return co_await write_mutex_->lock();
        }

        net::awaitable<boost::system::error_code>
        write_message_locked()
        {
            boost::system::error_code ec;
            if (header_sent_)
            {
                co_return ec;
            }

            prepare_payload();
            sr_ = std::make_unique<serializer_t>(msg_);
            last_header_done_ = false;

            static char empty_byte = 0;
            bool refill = true;
            while (!sr_->is_done())
            {
                if (refill)
                {
                    body::source::chunk_t chunk;
                    if (source_)
                    {
                        chunk = source_->next(ec);
                        if (ec)
                        {
                            last_header_done_ = sr_->is_header_done();
                            co_return ec;
                        }
                    }
                    if (chunk)
                    {
                        msg_.body().data = const_cast<void*>(chunk->first.data());
                        msg_.body().size = chunk->first.size();
                        msg_.body().more = chunk->second;
                    }
                    else
                    {
                        msg_.body().data = &empty_byte;
                        msg_.body().size = 0;
                        msg_.body().more = false;
                    }
                }

                co_await task_->write_some(*sr_, ec);
                if (ec == http::error::need_buffer)
                {
                    ec = {};
                    refill = true;
                }
                else if (ec)
                {
                    last_header_done_ = sr_->is_header_done();
                    co_return ec;
                }
                else
                {
                    refill = false;
                }
            }
            last_header_done_ = sr_->is_header_done();
            ec = {};
            co_return ec;
        }

        net::awaitable<boost::system::error_code>
        begin_stream_locked(stream_mode mode)
        {
            boost::system::error_code ec;
            if (header_sent_)
            {
                co_return ec;
            }

            if (mode == stream_mode::chunked)
            {
                msg_.body() = http::buffer_body::value_type {};
                msg_.chunked(true);

                // 调用方声明的 Content-Encoding 由流式写入侧逐块压缩。
                auto encoding = msg_[http::field::content_encoding];
                if (!encoding.empty() && compress::compressor_factory::instance().is_transform_encoding(encoding))
                {
                    encoder_ = std::make_unique<body::stream_encoder>();
                    encoder_->reset(encoding, ec);
                    if (ec)
                    {
                        msg_.keep_alive(false);
                        co_return ec;
                    }
                }
            }
            else
            {
                reset();
            }

            sr_ = std::make_unique<serializer_t>(msg_);
            co_await task_->write_header(*sr_, ec);
            if (ec)
            {
                msg_.keep_alive(false);
            }
            else
            {
                header_sent_ = true;
            }
            co_return ec;
        }

        net::awaitable<boost::system::error_code>
        write_some_locked(net::const_buffer const& data, bool more)
        {
            boost::system::error_code ec;
            if (!sr_)
            {
                co_return invalid_argument();
            }

            if (encoder_)
            {
                encoder_->consume_all();
                encoder_->feed(data, more, ec);
                if (ec)
                {
                    msg_.keep_alive(false);
                    co_return ec;
                }
                auto buffer = encoder_->buffer();
                if (buffer.size() == 0 && more)
                {
                    co_return ec;
                }
                static char const empty_byte = 0;
                if (buffer.size() == 0)
                {
                    msg_.body().data = const_cast<char*>(&empty_byte);
                    msg_.body().size = 0;
                }
                else
                {
                    msg_.body().data = const_cast<void*>(buffer.data());
                    msg_.body().size = buffer.size();
                }
                msg_.body().more = more;
                co_return co_await write_serialized(ec);
            }

            msg_.body().data = data.size() > 0 ? const_cast<void*>(data.data()) : nullptr;
            msg_.body().size = data.size();
            msg_.body().more = more;
            co_return co_await write_serialized(ec);
        }

        net::awaitable<boost::system::error_code>
        write_serialized(boost::system::error_code& ec)
        {
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

        message_t& msg_;
        Task* task_ = nullptr;
        net::any_io_executor executor_;
        std::optional<util::async_mutex> write_mutex_;

        state_t payload_;
        body::source_ptr source_;

        std::unique_ptr<serializer_t> sr_;
        std::unique_ptr<body::stream_encoder> encoder_;
        bool header_sent_ = false;
        bool last_header_done_ = false;
    };
} // namespace httplib::detail
