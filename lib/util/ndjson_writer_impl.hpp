#pragma once
#include "httplib/chunk_writer.hpp"
#include "httplib/ndjson_writer.hpp"
#include <boost/json/serialize.hpp>
#include <string>

namespace httplib {

class ndjson_writer_impl : public ndjson_writer
{
public:
    explicit ndjson_writer_impl(chunk_writer& cw)
        : cw_(cw)
    {
    }

    net::awaitable<void> write(const boost::json::value& value) override
    {
        auto line = boost::json::serialize(value);
        line += "\n";
        co_await cw_.write_chunk(std::move(line));
    }

    net::awaitable<void> close() override
    {
        co_await cw_.close();
    }

private:
    chunk_writer& cw_;
};

} // namespace httplib
