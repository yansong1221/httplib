#pragma once
#include "body/any_body.hpp"
#include "httplib/server/stream_writer.hpp"
#include "httplib/util/async_mutex.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "response_impl.hpp"
#include "stream/http_stream.hpp"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <memory>
#include <string>

namespace httplib::server
{

    class stream_writer_impl : public stream_writer
    {
      public:
        stream_writer_impl(response::impl& resp, http_stream& stream, std::chrono::steady_clock::duration write_timeout)
            : resp_(&resp)
            , stream_(&stream)
            , write_timeout_(write_timeout)
            , write_mutex_(stream.get_executor())
        {
        }

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
        write_header(http::status status,
                     http::fields const& headers,
                     mode m,
                     boost::system::error_code& ec) override
        {
            auto write_lock = co_await write_mutex_.lock();
            if (!write_lock)
            {
                ec = net::error::operation_aborted;
                co_return;
            }

            for (auto const& f : headers)
            {
                resp_->erase(f.name_string());
            }
            resp_->result(status);
            for (auto const& f : headers)
            {
                resp_->insert(f.name_string(), f.value());
            }
            stream_->expires_after(write_timeout_);

            if (m == mode::relay)
            {
                resp_->reset_content();

                // 代理转发：原样透传上游字节，不做二次压缩。
                relay_msg_ = std::make_unique<http::response<http::buffer_body>>(*resp_);
                relay_sr_ = std::make_unique<http::response_serializer<http::buffer_body>>(*relay_msg_);
                co_await http::async_write_header(*stream_, *relay_sr_, util::net_awaitable[ec]);
            }
            else
            {
                resp_->body() = body::buffer_body::value_type {};
                resp_->chunked(true);

                sr_ = std::make_unique<http::response_serializer<body::any_body>>(*resp_);
                co_await http::async_write_header(*stream_, *sr_, util::net_awaitable[ec]);
            }
            stream_->expires_never();
            if (ec)
            {
                resp_->keep_alive(false);
            }
            else
            {
                resp_->set_stream_header_sent(true);
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

            if (sr_)
            {
                auto& body = std::get<body::buffer_body::value_type>(sr_->get().body());
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
            stream_->expires_after(write_timeout_);
            co_await http::async_write(*stream_, sr, util::net_awaitable[ec]);
            stream_->expires_never();
            if (ec == http::error::need_buffer)
            {
                ec = {};
            }
            else if (ec)
            {
                resp_->keep_alive(false);
            }
        }

      private:
        response::impl* resp_;
        http_stream* stream_;
        std::chrono::steady_clock::duration write_timeout_;
        // 串行化所有写入，保证一次只有一个协程操作序列化器/流。
        util::async_mutex write_mutex_;
        // 直连流式：any_body 序列化（支持 Content-Encoding 压缩）。
        std::unique_ptr<http::response_serializer<body::any_body>> sr_;
        // 代理转发：beast buffer_body 原样透传。
        std::unique_ptr<http::response<http::buffer_body>> relay_msg_;
        std::unique_ptr<http::response_serializer<http::buffer_body>> relay_sr_;
    };

} // namespace httplib::server
