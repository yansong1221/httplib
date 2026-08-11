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

        net::awaitable<void>
        begin()
        {
            http::fields headers;
            headers.set(http::field::content_type, "application/x-ndjson");
            if (auto ec = co_await cw_->write_header(http::status::ok, headers, false); ec)
            {
                throw boost::system::system_error(ec);
            }
        }

        net::awaitable<void>
        write(boost::json::value const& value, bool more) override
        {
            auto line = boost::json::serialize(value);
            line += "\n";
            if (auto ec = co_await cw_->write_body(net::buffer(line), more); ec)
            {
                throw boost::system::system_error(ec);
            }
        }

      private:
        server::chunk_writer* cw_;
    };

} // namespace httplib::server
