#pragma once
#include "httplib/chunk_reader.hpp"
#include "httplib/ndjson_reader.hpp"
#include <boost/json/parse.hpp>
#include <string>

namespace httplib {

class ndjson_reader_impl : public ndjson_reader
{
public:
    explicit ndjson_reader_impl(chunk_reader& reader)
        : reader_(reader)
    {
    }

    net::awaitable<boost::json::value> read() override
    {
        if (reader_.is_done())
            co_return boost::json::value {};

        auto chunk = co_await reader_.read_chunk();
        if (chunk.empty())
            co_return boost::json::value {};

        auto sv = std::string_view(chunk);
        if (sv.back() == '\n')
            sv.remove_suffix(1);
        if (!sv.empty() && sv.back() == '\r')
            sv.remove_suffix(1);

        co_return boost::json::parse(sv);
    }

    bool is_done() const override
    {
        return reader_.is_done();
    }

private:
    chunk_reader& reader_;
};

} // namespace httplib
