#pragma once
#include "httplib/server/ndjson_writer.hpp"
#include "httplib/server/stream_writer.hpp"
#include <boost/json/serialize.hpp>
#include <memory>
#include <string>
#include <utility>
#include "beast_alias.hpp"

namespace httplib::server
{

    class ndjson_writer_impl : public server::ndjson_writer
    {
      public:
        // 共享持有 stream_writer，理由同 sse_writer_impl。
        explicit ndjson_writer_impl(std::shared_ptr<server::stream_writer> cw) : cw_(std::move(cw)) {}

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
            headers headers;
            headers.set(field::content_type, "application/x-ndjson");
            co_await cw_->write_header(status::ok, headers, stream_writer::mode::chunked, ec);
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
        std::shared_ptr<server::stream_writer> cw_;
    };

} // namespace httplib::server
