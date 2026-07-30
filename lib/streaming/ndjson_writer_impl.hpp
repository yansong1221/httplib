#pragma once
#include "httplib/streaming/chunk_writer.hpp"
#include "httplib/streaming/ndjson_writer.hpp"
#include <boost/json/serialize.hpp>
#include <string>

namespace httplib::streaming {

class ndjson_writer_impl : public httplib::ndjson_writer
{
public:
    explicit ndjson_writer_impl(httplib::chunk_writer& cw)
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
    httplib::chunk_writer& cw_;
};

} // namespace httplib::streaming
