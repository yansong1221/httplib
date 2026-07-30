#pragma once
#include "streaming/chunk_reader_core.hpp"
#include "httplib/streaming/chunk_reader.hpp"
#include "stream/http_stream.hpp"
#include <boost/beast/http/parser.hpp>
#include <chrono>

namespace httplib::streaming {

template<bool isRequest>
class chunk_reader_impl : public httplib::chunk_reader, public chunk_reader_core<isRequest>
{
    using core_type = chunk_reader_core<isRequest>;

public:
    chunk_reader_impl(http_stream& stream,
                      beast::flat_buffer& buffer,
                      http::parser<isRequest, body::any_body>& parser,
                      std::chrono::steady_clock::duration read_timeout)
    {
        core_type::setup(stream, buffer, parser, read_timeout);
    }

    net::awaitable<std::string_view> read_chunk() override
    {
        co_return co_await core_type::read_chunk();
    }

    bool is_done() const override
    {
        return core_type::is_done();
    }
};

} // namespace httplib::streaming
