#pragma once
#include "body/codec.hpp"
#include "body/source.hpp"
#include "compress/compressor.hpp"
#include "html/html.h"
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
#include <format>
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
          - 流式写：begin_stream() + write_compressed(data, more) / write_raw(data, more)。

        业务数据由各 source 自持（见 body/source.hpp），本对象只持有 @ref source_ 产出字节。
        HTTP message 由本对象以成员 @ref msg_ 自持；对外只暴露 @ref base()（start-line +
        headers，即 Beast 的 header_type），body 与 serializer 状态保持私有。

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
            source_ = std::make_shared<body::string_source>(std::move(data));
        }

        void
        set_json(boost::json::value data)
        {
            reset();
            msg_.set(http::field::content_type, "application/json; charset=utf-8");
            msg_.set(http::field::cache_control, "no-store");

            source_ = std::make_shared<body::json_source>(std::move(data));
        }

        void
        set_query_params(httplib::query_params data)
        {
            reset();
            msg_.set(http::field::content_type, "application/x-www-form-urlencoded");
            source_ = std::make_shared<body::query_params_source>(std::move(data));
        }

        void
        set_form_data(httplib::form_data data)
        {
            reset();
            msg_.set(http::field::content_type, "multipart/form-data; boundary=" + data.boundary);
            source_ = std::make_shared<body::form_data_source>(std::move(data));
        }

        void
        set_file(std::ifstream file, std::string content_type, httplib::html::http_ranges ranges)
        {
            reset();
            if (ranges.empty() || ranges.size() == 1)
            {
                msg_.set(http::field::content_type, content_type);
                source_ = std::make_shared<body::file_source>(std::move(file),
                                                              std::move(ranges),
                                                              std::move(content_type),
                                                              "");
            }
            else
            {
                std::string boundary = httplib::html::generate_boundary();
                msg_.set(http::field::content_type, std::format("multipart/byteranges; boundary={}", boundary));
                source_ = std::make_shared<body::file_source>(std::move(file),
                                                              std::move(ranges),
                                                              std::move(content_type),
                                                              std::move(boundary));
            }
        }
        void
        set_empty()
        {
            reset();
            // 空 body 也挂一个 source（长度 0、不产字节），使分帧统一走 source 长度这条路径，
            // 而不是在 prepare_payload() 里再对"无 source"分一种情况。空 source 无状态，共享单例。
            source_ = body::shared_empty_source();
        }

        /// 丢弃 body payload 与编码器状态，但保留 header（含 Content-Length 及其分帧）。
        ///
        /// 只被 @ref reset() 调用（换新 body）。注意它**不**适用于 HEAD：丢掉 source 之后
        /// @ref prepare_payload 无法按真实 source 重算分帧。HEAD 请用
        /// @ref write_message 的 `headers_only`。
        void
        discard_body()
        {
            reset_serializer();
            source_.reset();
            msg_.body() = http::buffer_body::value_type {};
            encoder_.reset();
        }

        /// 准备写入一个新 body：丢掉 body payload 与上一个 body 留下的 Content-Length。
        ///
        /// prepare_payload() 见到已存在的 Content-Length 会直接沿用，"旧长度 + 新 body"会把
        /// 对端挂在那儿等永远不会到达的字节，所以换 body 必须连同它一起丢掉；分帧会在下次
        /// prepare_payload() 按当前 body 重算。要指定 Content-Length（HEAD 回显、multipart 等）
        /// 请在 set_* 之后再设。
        void
        reset()
        {
            discard_body();
            msg_.erase(http::field::content_length);
        }

        // ---- 分帧 ----

        /// 分帧：优先采用 source 已知长度；source 报不出长度（multipart / 未知）时保留调用方
        /// 显式设的 Content-Length，否则交给 Beast 分帧；完全没有 source（HEAD 丢 body 后或
        /// 未设过 body）时保留已有 Content-Length，没有才记 CL:0。
        void
        prepare_payload()
        {
            // 转换编码（gzip/br/...）会改写字节长度，明文 Content-Length 不再成立。
            // 分帧交给 Beast 的 prepare_payload()：它只在 HTTP/1.1 上选 chunked，
            // 而 HTTP/1.0 没有 chunked 传输编码，只能不写 Content-Length、靠关连接定界 body。
            // 空 body（长度已知为 0）无需编码，交给下面的长度分支记 CL:0。
            if (!msg_[http::field::content_encoding].empty() && source_ && source_->content_length() != 0)
            {
                msg_.erase(http::field::content_length);
                msg_.body() = http::buffer_body::value_type {};
                msg_.prepare_payload();
                return;
            }

            if (source_)
            {
                if (auto length = source_->content_length())
                {
                    // source 自己知道长度：以它为准，顺带覆盖上一个 body 可能残留的 Content-Length。
                    msg_.chunked(false);
                    msg_.content_length(*length);
                    return;
                }
                // source 报不出长度：调用方显式设了 Content-Length 就沿用，否则交给 Beast 分帧。
                if (msg_.has_content_length())
                {
                    msg_.chunked(false);
                    return;
                }
                msg_.prepare_payload();
                return;
            }

            // 无 body：保留调用方显式设的 Content-Length（HEAD 回显 GET 的长度），否则记 CL:0。
            if (!msg_.has_content_length())
            {
                msg_.chunked(false);
                msg_.content_length(0);
            }
        }

        void
        chunked(bool value)
        {
            msg_.chunked(value);
        }

        // ---- 状态访问 ----

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

        /// 头部是否已写出（client 死连接重试判定用）。
        bool
        header_done() const
        {
            return sr_ && sr_->is_header_done();
        }

        // ---- 整消息写 ----

        /// 整消息写。`headers_only` 为真时只写 header：一个 body 字节都不写，连 chunked 的
        /// "0\r\n\r\n" 终止块也不写。
        ///
        /// HEAD 响应用它。头仍由 @ref prepare_payload 从同一个 source 算出，所以与同一 URL 的
        /// GET 逐字相同（含 Content-Length / Transfer-Encoding），只是没有 body——这正是
        /// RFC 9112 §6.3 对 HEAD 的要求。比"丢掉 source 再走收尾写"干净：那样要么把
        /// Content-Length 覆盖成 0（谎报资源为空），要么凭空压出一个空 body。
        net::awaitable<boost::system::error_code>
        write_message(bool headers_only = false)
        {
            auto lock = co_await lock_write();
            if (!lock)
            {
                co_return aborted();
            }
            co_return co_await write_message_locked(headers_only);
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

        /// 流式写一块 body，驱动到该块全部写完才返回：按 Content-Encoding 自动压缩
        /// （无转换编码时退化为 raw）。
        net::awaitable<boost::system::error_code>
        write_compressed(net::const_buffer const& data, bool more)
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
            co_return co_await write_compressed_locked(data, more);
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
        write_message_locked(bool headers_only)
        {
            boost::system::error_code ec;
            if (sr_ && sr_->is_header_done())
            {
                co_return invalid_argument();
            }

            prepare_payload();
            // 序列化器惰性就绪：统一经 serializer() 这一处构造。
            auto& sr = serializer();

            // 先只写头：失败时 source_ 尚未被消费，调用方（如 client 死连接重试）
            // 可 reset_serializer() 后安全重发，不会丢 body。
            co_await task_->write_header(sr, ec);
            if (ec)
            {
                msg_.keep_alive(false);
                co_return ec;
            }

            // HEAD：头已写完就收工。body 留在 source_ 里不消费，序列化器停在 header_done——
            // 服务端不依赖 is_done()（只有客户端复用请求对象时才依赖，见 prepare_for_send()），
            // 所以这样即可，不必也不能走下面的收尾写：chunked 会写出 "0\r\n\r\n" 终止块，
            // 那几字节就是 body。
            if (headers_only)
            {
                co_return ec;
            }

            // body 为空时，prepare_payload() 记的是 CL:0 + chunked(false)，线上不该有任何 body
            // 字节。但仍要把序列化器驱动到 done（客户端复用请求对象靠这个判定，见
            // prepare_for_send()），所以照常写一个空 body——只是必须走 write_raw_locked，绝不能走
            // write_compressed_locked：后者会冲刷编码器，产出一个 20 字节的空载荷 gzip 流
            // （RFC 1952）并写下去，与 CL:0 矛盾。对端按 CL:0 读 0 字节，剩下的流字节被当成下一个
            // 响应的开头（实测 beast.http:14 "bad version"），这条 keep-alive 连接就废了。
            //
            // "空"有两种形态，两个分支都得判：
            //   - source_ 为空指针：discard_body() 之后的状态，是有意保留的（HEAD 靠它保住显式
            //     Content-Length，见 prepare_payload 与 discard_body 的注释），此时压根没有
            //     content_length() 可问。实测「只设 Content-Type、不设 body」就会走到这里。
            //   - 挂了 source 但长度为 0：empty_source（set_empty() 用它代表无内容）与
            //     string_source("") 之类。empty_source::content_length() 就返回 0。
            // Content-Encoding 不影响这个判断：空 body 照样可能带它（session.cpp 协商时不看 body
            // 是否为空），而正因为没有字节可编码，才更不能凭空造出一个压缩流。
            // 注意不能靠 sr.is_done() 判断——实测头写完后它仍是 false，Beast 要再 consume 一次
            // 才认到 body 结束。
            if (!source_ || source_->content_length() == 0)
            {
                co_return co_await write_raw_locked(net::const_buffer {}, false);
            }

            // 逐块拉取 source，经统一原语落地；source 读尽后以空 body + more=false 收尾
            // （压缩时正是这一步冲刷编码器并写出 chunked 终止块）。这一步同时把序列化器驱动到
            // is_done()——请求对象要靠这个判定重置序列化器后复用（见 prepare_for_send()），
            // 所以不能因为"没有 body"就整个跳过。
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

            // 只重置 body 与编码器状态；chunked / content-length 由调用方事先在 base() 上配置
            // （relay 透传上游头、chunked 由调用方置位），故这里不能动 header。
            discard_body();
            auto& sr = serializer();
            co_await task_->write_header(sr, ec);
            if (ec)
            {
                msg_.keep_alive(false);
            }
            co_return ec;
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
            // 统一经 serializer() 取序列化器：所有调用方（write_raw / write_compressed /
            // write_message_locked）都已写过 header，sr_ 必非空，这里只是不依赖调用顺序。
            co_await task_->write(serializer(), ec);
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

            // 上一轮产物已被写尽（write_raw_locked 驱动到消费完），可安全清理后喂入新明文。
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

        body::source_ptr source_;

        std::unique_ptr<serializer_t> sr_;
        /// Content-Encoding 编码器，按需懒初始化（仅在需要压缩时存在）。
        std::unique_ptr<body::stream_encoder> encoder_;
    };
} // namespace httplib::detail
