#pragma once
#include "httplib/streaming/chunk_writer.hpp"
#include "stream/http_stream.hpp"
#include <atomic>
#include <boost/asio/write.hpp>
#include <boost/beast/http/chunk_encode.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace httplib::streaming {

class chunk_writer_impl : public httplib::chunk_writer
{
public:
    chunk_writer_impl(http_stream& stream, std::chrono::steady_clock::duration write_timeout)
        : stream_(&stream)
        , write_timeout_(write_timeout)
    {
    }

    net::awaitable<void> write_chunk(std::string_view data) override
    {
        std::string chunk(data);
        http::chunk_body chunk_b(net::buffer(chunk));
        stream_->expires_after(write_timeout_);
        co_await net::async_write(*stream_, chunk_b, net::use_awaitable);
        stream_->expires_never();
    }

    net::awaitable<void> close() override
    {
        if (closed_.exchange(true))
            co_return;
        http::chunk_last last_chunk;
        stream_->expires_after(write_timeout_);
        co_await net::async_write(*stream_, last_chunk, net::use_awaitable);
        stream_->expires_never();
    }

private:
    http_stream* stream_;
    std::chrono::steady_clock::duration write_timeout_;
    std::atomic<bool> closed_ = false;
};

} // namespace httplib::streaming
