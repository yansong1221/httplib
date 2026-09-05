#pragma once
#include "body/any_body.hpp"
#include "httplib/server/stream_writer.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "response_impl.hpp"
#include "stream/http_stream.hpp"
#include <boost/asio/strand.hpp>
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
            , strand_(net::make_strand(stream.get_executor()))
        {
        }

        bool
        has_header() const override
        {
            return header_sent_;
        }

        net::awaitable<boost::system::error_code>
        write_header(http::status status, http::fields const& headers, bool relay) override
        {
            co_await boost::asio::post(strand_);

            for (auto const& f : headers)
            {
                resp_->erase(f.name());
            }
            resp_->result(status);
            for (auto const& f : headers)
            {
                resp_->insert(f.name(), f.value());
            }

            boost::system::error_code ec;
            stream_->expires_after(write_timeout_);

            if (relay)
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
                header_sent_ = true;
            }
            co_return ec;
        }

        net::awaitable<boost::system::error_code>
        write_body(net::const_buffer const& data, bool more) override
        {
            co_await boost::asio::post(strand_);

            if (!sr_ && !relay_sr_)
            {
                throw boost::system::system_error(
                    boost::system::errc::make_error_code(boost::system::errc::invalid_argument));
            }

            if (sr_)
            {
                auto& body = std::get<body::buffer_body::value_type>(sr_->get().body());
                body.data = (void*)data.data();
                body.size = data.size();
                body.more = more;
                co_return co_await write_buffer(*sr_);
            }

            relay_msg_->body().data = (void*)data.data();
            relay_msg_->body().size = data.size();
            relay_msg_->body().more = more;
            co_return co_await write_buffer(*relay_sr_);
        }

      private:
        template <typename Serializer>
        net::awaitable<boost::system::error_code>
        write_buffer(Serializer& sr)
        {
            boost::system::error_code ec;
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
            co_return ec;
        }

      private:
        net::strand<http_stream::executor_type> strand_;
        response::impl* resp_;
        http_stream* stream_;
        std::chrono::steady_clock::duration write_timeout_;
        // 直连流式：any_body 序列化（支持 Content-Encoding 压缩）。
        std::unique_ptr<http::response_serializer<body::any_body>> sr_;
        // 代理转发：beast buffer_body 原样透传。
        std::unique_ptr<http::response<http::buffer_body>> relay_msg_;
        std::unique_ptr<http::response_serializer<http::buffer_body>> relay_sr_;
        bool header_sent_ = false;
    };

} // namespace httplib::server
