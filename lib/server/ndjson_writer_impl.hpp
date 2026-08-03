#pragma once
#include "httplib/server/chunk_writer.hpp"
#include "httplib/server/ndjson_writer.hpp"
#include <boost/json/serialize.hpp>
#include <string>

namespace httplib::server
{

    class ndjson_writer_impl : public server::ndjson_writer
    {
      public:
        explicit ndjson_writer_impl(server::chunk_writer* cw) : cw_(cw) {}

        net::awaitable<boost::system::error_code>
        begin()
        {
            http::fields headers;
            headers.set(http::field::content_type, "application/x-ndjson");
            auto ec = co_await cw_->write_header(http::status::ok, headers, false);
            if (ec)
            {
                co_return ec;
            }
            co_return boost::system::error_code {};
        }

        net::awaitable<void>
        write(boost::json::value const& value, bool more) override
        {
            auto line = boost::json::serialize(value);
            line += "\n";
            co_await cw_->write_body(net::buffer(line), more);
        }

      private:
        server::chunk_writer* cw_;
    };

} // namespace httplib::server
