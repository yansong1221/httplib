#pragma once
#include "httplib/streaming/buffer_body_writer.hpp"
#include "stream/http_stream.hpp"
#include <atomic>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <chrono>
#include <memory>

namespace httplib::streaming {

class buffer_body_writer_impl : public buffer_body_writer
{
public:
    buffer_body_writer_impl(http_stream& stream,
                            http::response<http::buffer_body>& msg,
                            http::response_serializer<http::buffer_body>& sr,
                            std::chrono::steady_clock::duration write_timeout)
        : stream_(&stream)
        , msg_(&msg)
        , sr_(&sr)
        , write_timeout_(write_timeout)
    {
    }

    net::awaitable<void> write(const net::const_buffer& data, bool more) override
    {
        msg_->body().data = (void*)data.data();
        msg_->body().size = data.size();
        msg_->body().more = more;

        boost::system::error_code ec;
        stream_->expires_after(write_timeout_);
        co_await http::async_write(*stream_, *sr_, util::net_awaitable[ec]);
        if (ec == http::error::need_buffer)
            ec = {};
        stream_->expires_never();
        if (ec)
            throw boost::system::system_error(ec);
    }

private:
    http_stream* stream_;
    http::response<http::buffer_body>* msg_;
    http::response_serializer<http::buffer_body>* sr_;
    std::chrono::steady_clock::duration write_timeout_;
    std::atomic<bool> closed_ = false;
};

} // namespace httplib::streaming
