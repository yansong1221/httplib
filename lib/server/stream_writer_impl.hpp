#pragma once
#include "body/codec.hpp"
#include "compress/compressor.hpp"
#include "httplib/server/stream_writer.hpp"
#include "httplib/util/async_mutex.hpp"
#include "response_impl.hpp"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/serializer.hpp>
#include <memory>
#include <string>

namespace httplib::server
{

    class stream_writer_impl : public stream_writer
    {
      public:
        stream_writer_impl(response::impl& resp) : resp_(resp), write_mutex_(resp_.task_->executor()) {}

        net::awaitable<void>
        write_header(http::status status, http::fields const& headers, mode m) override
        {
            boost::system::error_code ec;
            co_await write_header(status, headers, m, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        write_header(http::status status, http::fields const& headers, mode m, boost::system::error_code& ec) override
        {
            auto write_lock = co_await write_mutex_.lock();
            if (!write_lock)
            {
                ec = net::error::operation_aborted;
                co_return;
            }

            for (auto const& f : headers)
            {
                resp_.erase(f.name_string());
            }
            resp_.result(status);
            for (auto const& f : headers)
            {
                resp_.insert(f.name_string(), f.value());
            }

            if (m == mode::relay)
            {
                resp_.reset_content();

                // 代理转发：原样透传上游字节，不做二次压缩。
                relay_msg_ = std::make_unique<http::response<http::buffer_body>>(resp_);
                relay_sr_ = std::make_unique<http::response_serializer<http::buffer_body>>(*relay_msg_);
                co_await resp_.task_->write_header(*relay_sr_, ec);
            }
            else
            {
                resp_.body() = http::buffer_body::value_type {};
                resp_.chunked(true);

                // 调用方声明的 Content-Encoding 由流式写入侧逐块压缩。
                auto encoding = resp_[http::field::content_encoding];
                if (!encoding.empty()
                    && compress::compressor_factory::instance().is_transform_encoding(encoding))
                {
                    encoder_ = std::make_unique<body::stream_encoder>();
                    encoder_->reset(encoding, ec);
                    if (ec)
                    {
                        resp_.keep_alive(false);
                        co_return;
                    }
                }

                sr_ = std::make_unique<http::response_serializer<http::buffer_body>>(resp_);
                co_await resp_.task_->write_header(*sr_, ec);
            }
            if (ec)
            {
                resp_.keep_alive(false);
            }
            else
            {
                resp_.set_stream_header_sent(true);
            }
        }
        net::awaitable<void>
        write_body(net::const_buffer const& data, bool more) override
        {
            boost::system::error_code ec;
            co_await write_body(data, more, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        write_body(net::const_buffer const& data, bool more, boost::system::error_code& ec) override
        {
            auto write_lock = co_await write_mutex_.lock();
            if (!write_lock)
            {
                ec = net::error::operation_aborted;
                co_return;
            }

            if (!sr_ && !relay_sr_)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
                co_return;
            }

            if (sr_ && encoder_)
            {
                encoder_->consume_all();
                encoder_->feed(data, more, ec);
                if (ec)
                {
                    resp_.keep_alive(false);
                    co_return;
                }
                auto buffer = encoder_->buffer();
                if (buffer.size() == 0 && more)
                {
                    co_return;
                }
                static char const empty_byte = 0;
                auto& body = resp_.body();
                if (buffer.size() == 0)
                {
                    body.data = const_cast<char*>(&empty_byte);
                    body.size = 0;
                }
                else
                {
                    body.data = const_cast<void*>(buffer.data());
                    body.size = buffer.size();
                }
                body.more = more;
                co_await write_buffer(*sr_, ec);
                co_return;
            }

            if (sr_)
            {
                auto& body = resp_.body();
                body.data = (void*)data.data();
                body.size = data.size();
                body.more = more;
                co_await write_buffer(*sr_, ec);
                co_return;
            }

            relay_msg_->body().data = data.size() > 0 ? const_cast<void*>(data.data()) : nullptr;
            relay_msg_->body().size = data.size();
            relay_msg_->body().more = more;
            co_await write_buffer(*relay_sr_, ec);
            co_return;
        }

      private:
        template <typename Serializer>
        net::awaitable<void>
        write_buffer(Serializer& sr, boost::system::error_code& ec)
        {
            co_await resp_.task_->write(sr, ec);
            if (ec == http::error::need_buffer)
            {
                ec = {};
            }
            else if (ec)
            {
                resp_.keep_alive(false);
            }
        }

      private:
        response::impl& resp_;
        // 串行化所有写入，保证一次只有一个协程操作序列化器/流。
        util::async_mutex write_mutex_;
        // 直连流式：buffer_body 序列化（调用方逐块喂入）。
        std::unique_ptr<http::response_serializer<http::buffer_body>> sr_;
        // 直连流式压缩（Content-Encoding），无则逐块透传。
        std::unique_ptr<body::stream_encoder> encoder_;
        // 代理转发：beast buffer_body 原样透传。
        std::unique_ptr<http::response<http::buffer_body>> relay_msg_;
        std::unique_ptr<http::response_serializer<http::buffer_body>> relay_sr_;
    };

} // namespace httplib::server
