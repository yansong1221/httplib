#pragma once
#include "httplib/streaming/chunk_reader.hpp"
#include "httplib/streaming/sse_reader.hpp"
#include "streaming/sse_event_parser.hpp"

namespace httplib::streaming {

class sse_reader_impl : public httplib::sse_reader
{
public:
    explicit sse_reader_impl(httplib::chunk_reader& reader)
        : reader_(reader)
    {
    }

    net::awaitable<sse_event> read_event() override
    {
        while (!parser_.has_event() && !reader_.is_done()) {
            auto chunk = co_await reader_.read_chunk();
            if (chunk.empty())
                break;
            parser_.feed(chunk);
        }
        if (parser_.has_event())
            co_return parser_.next();
        co_return sse_event {};
    }

    bool is_done() const override { return reader_.is_done() && !parser_.has_event(); }

private:
    httplib::chunk_reader& reader_;
    detail::sse_event_parser parser_;
};

} // namespace httplib::streaming
