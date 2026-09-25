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

        业务数据写入本对象的 @ref payload_，source_ 引用它产出字节。message 由本对象自持，
        Beast 的 `http::buffer_body::writer` 只接受 const message，serializer 通过自持的
        message 引用完成序列化。

        数据落地由构造期/attach 注入的 Task 提供（方向仅由 IsRequest 决定）：

            task->write_header(serializer, ec);   // 只写头
            task->write_some(serializer, ec);     // 单步写（整消息驱动用）
            task->write(serializer, ec);          // 写到 need_buffer/完成（流式块用）
    */
    template <bool IsRequest, typename Task>
    class body_writer : public http::message<IsRequest, http::buffer_body, http::fields>
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

        explicit body_writer(Task* task = nullptr, net::any_io_executor ex = {}) : task_(task), executor_(std::move(ex))
        {
        }

        body_writer(body_writer const&) = delete;
        body_writer& operator=(body_writer const&) = delete;

        void
        attach(Task* task, net::any_io_executor ex)
        {
            task_ = task;
            executor_ = std::move(ex);
            write_mutex_.reset();
        }

        void
        set_string(std::string data, std::string_view content_type)
        {
            reset();
            this->set(http::field::content_type, content_type);
            payload_.set<std::string>(std::move(data));
            source_ = std::make_unique<body::string_source>(payload_.as<std::string>());
        }

        void
        set_json(boost::json::value data, std::string_view content_type, bool no_store = false)
        {
            reset();
            this->set(http::field::content_type, content_type);
            if (no_store)
            {
                this->set(http::field::cache_control, "no-store");
            }
            payload_.set<boost::json::value>(std::move(data));
            source_ = std::make_unique<body::json_source>(payload_.as<boost::json::value>());
        }

        void
        set_query_params(httplib::query_params data)
        {
            reset();
            this->set(http::field::content_type, "application/x-www-form-urlencoded");
            payload_.set<httplib::query_params>(std::move(data));
            source_ = std::make_unique<body::query_params_source>(payload_.as<httplib::query_params>());
        }

        void
        set_form_data(httplib::form_data data)
        {
            reset();
            this->set(http::field::content_type, "multipart/form-data; boundary=" + data.boundary);
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
            encoder_.reset();
            this->body() = http::buffer_body::value_type {};
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

        /// 未显式设置 Content-Length 时：优先用 source 已知长度，其次空 source 记 CL:0，否则走 chunked。
        void
        prepare_payload()
        {
            if (this->has_content_length())
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
                message_t::chunked(false);
                this->content_length(*length);
                return;
            }
            message_t::prepare_payload();
        }

        void
        chunked(bool value)
        {
            message_t::chunked(value);
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

        serializer_t&
        serializer()
        {
            if (!sr_)
            {
                sr_ = std::make_unique<serializer_t>(*this);
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
                write_mutex_ = std::make_unique<util::async_mutex>(executor_);
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
            if (!sr_)
            {
                sr_ = std::make_unique<serializer_t>(*this);
            }

            prepare_payload();
            auto& sr = serializer();

            bool refill = true;
            while (!sr.is_done())
            {
                if (refill)
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
                        this->body().data = const_cast<void*>(chunk->first.data());
                        this->body().size = chunk->first.size();
                        this->body().more = chunk->second;
                    }
                    else
                    {
                        this->body().data = nullptr;
                        this->body().size = 0;
                        this->body().more = false;
                    }
                }

                co_await task_->write_some(sr, ec);
                if (ec == http::error::need_buffer)
                {
                    ec = {};
                    refill = true;
                }
                else if (ec)
                {
                    co_return ec;
                }
                else
                {
                    refill = false;
                }
            }
            ec = {};
            co_return ec;
        }

        net::awaitable<boost::system::error_code>
        begin_stream_locked(stream_mode mode)
        {
            boost::system::error_code ec;
            if (sr_ && sr_->is_header_done())
            {
                co_return invalid_argument();
            }
            if (!sr_)
            {
                sr_ = std::make_unique<serializer_t>(*this);
            }

            if (mode == stream_mode::chunked)
            {
                this->body() = http::buffer_body::value_type {};
                message_t::chunked(true);

                // 调用方声明的 Content-Encoding 由流式写入侧逐块压缩。
                auto encoding = (*this)[http::field::content_encoding];
                if (!encoding.empty() && compress::compressor_factory::instance().is_transform_encoding(encoding))
                {
                    encoder_ = std::make_unique<body::stream_encoder>();
                    encoder_->reset(encoding, ec);
                    if (ec)
                    {
                        this->keep_alive(false);
                        co_return ec;
                    }
                }
            }
            else
            {
                reset();
            }

            auto& sr = serializer();
            co_await task_->write_header(sr, ec);
            if (ec)
            {
                this->keep_alive(false);
            }
            co_return ec;
        }

        net::awaitable<boost::system::error_code>
        write_some_locked(net::const_buffer const& data, bool more)
        {
            boost::system::error_code ec;
            if (!sr_ || !sr_->is_header_done() || sr_->is_done())
            {
                co_return invalid_argument();
            }

            if (encoder_)
            {
                encoder_->consume_all();
                encoder_->feed(data, more, ec);
                if (ec)
                {
                    this->keep_alive(false);
                    co_return ec;
                }
                auto buffer = encoder_->buffer();
                if (buffer.size() == 0 && more)
                {
                    co_return ec;
                }
                if (buffer.size() == 0)
                {
                    this->body().data = nullptr;
                    this->body().size = 0;
                }
                else
                {
                    this->body().data = const_cast<void*>(buffer.data());
                    this->body().size = buffer.size();
                }
                this->body().more = more;
                co_return co_await write_serialized(ec);
            }

            this->body().data = data.size() > 0 ? const_cast<void*>(data.data()) : nullptr;
            this->body().size = data.size();
            this->body().more = more;
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
                this->keep_alive(false);
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

        std::unique_ptr<serializer_t> sr_;
        Task* task_ = nullptr;
        net::any_io_executor executor_;
        std::unique_ptr<util::async_mutex> write_mutex_;

        state_t payload_;
        body::source_ptr source_;

        std::unique_ptr<body::stream_encoder> encoder_;
    };
} // namespace httplib::detail
