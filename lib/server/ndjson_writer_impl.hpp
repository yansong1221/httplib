#pragma once
#include "httplib/server/ndjson_writer.hpp"
#include "httplib/server/stream_writer.hpp"
#include <boost/json/serialize.hpp>
#include <string>

namespace httplib::server
{

    class ndjson_writer_impl : public server::ndjson_writer
    {
      public:
        explicit ndjson_writer_impl(server::stream_writer* cw) : cw_(cw) {}

        net::awaitable<void>
        begin() override
        {
            boost::system::error_code ec;
            co_await begin(ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        begin(boost::system::error_code& ec) override
        {
            http::fields headers;
            headers.set(http::field::content_type, "application/x-ndjson");
            co_await cw_->write_header(http::status::ok, headers, stream_writer::mode::chunked, ec);
        }

        net::awaitable<void>
        write(boost::json::value const& value, bool more) override
        {
            boost::system::error_code ec;
            co_await write(value, more, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        write(boost::json::value const& value, bool more, boost::system::error_code& ec) override
        {
            auto line = boost::json::serialize(value);
            line += "\n";
            co_await cw_->write_body(net::buffer(line), more, ec);
        }

      private:
        server::stream_writer* cw_;
    };

} // namespace httplib::server
