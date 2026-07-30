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
        for (;;) {
            auto lf = buf_.find('\n');
            if (lf == std::string::npos) {
                if (reader_.is_done())
                    co_return boost::json::value {};
                auto chunk = co_await reader_.read_chunk();
                if (chunk.empty())
                    co_return boost::json::value {};
                buf_.append(chunk);
                continue;
            }

            auto line = buf_.substr(0, lf);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            buf_.erase(0, lf + 1);

            if (line.empty())
                continue;

            co_return boost::json::parse(line);
        }
    }

    bool is_done() const override
    {
        return reader_.is_done() && buf_.find('\n') == std::string::npos;
    }

private:
    chunk_reader& reader_;
    std::string buf_;
};

} // namespace httplib
