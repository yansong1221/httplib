#pragma once
#include "httplib/server/chunk_writer.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "response_impl.hpp"
#include "stream/http_stream.hpp"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <memory>

namespace httplib::server
{

    class chunk_writer_impl : public chunk_writer
    {
      public:
        chunk_writer_impl(response::impl& resp, http_stream& stream, std::chrono::steady_clock::duration write_timeout)
            : resp_(&resp)
            , stream_(&stream)
            , write_timeout_(write_timeout)
        {
        }

        net::awaitable<boost::system::error_code>
        write_header(http::status status, http::fields const& headers, bool relay) override
        {
            resp_->result(status);
            for (auto const& f : headers)
            {
                resp_->set(f.name_string(), f.value());
            }
            resp_->reset_content();
            if (!relay)
            {
                resp_->chunked(true);
            }

            msg_ = std::make_unique<http::response<http::buffer_body>>(*resp_);
            sr_ = std::make_unique<http::response_serializer<http::buffer_body>>(*msg_);

            boost::system::error_code ec;
            stream_->expires_after(write_timeout_);
            co_await http::async_write_header(*stream_, *sr_, util::net_awaitable[ec]);
            stream_->expires_never();
            if (ec)
            {
                resp_->keep_alive(false);
            }
            co_return ec;
        }

        net::awaitable<boost::system::error_code>
        write_body(net::const_buffer const& data, bool more) override
        {
            msg_->body().data = (void*)data.data();
            msg_->body().size = data.size();
            msg_->body().more = more;

            boost::system::error_code ec;
            stream_->expires_after(write_timeout_);
            co_await http::async_write(*stream_, *sr_, util::net_awaitable[ec]);
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
        response::impl* resp_;
        http_stream* stream_;
        std::chrono::steady_clock::duration write_timeout_;
        std::unique_ptr<http::response<http::buffer_body>> msg_;
        std::unique_ptr<http::response_serializer<http::buffer_body>> sr_;
    };

} // namespace httplib::server
