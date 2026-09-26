#pragma once
#include "httplib/server/stream_writer.hpp"
#include "response_impl.hpp"
#include <boost/beast/http/field.hpp>
#include <boost/system/error_code.hpp>
#include <string>

namespace httplib::server
{

    class stream_writer_impl : public stream_writer
    {
      public:
        explicit stream_writer_impl(response::impl& resp) : resp_(resp) {}

        net::awaitable<void>
        write_header(http::status status, http::fields const& headers, mode m) override
        {
            boost::system::error_code ec;
            co_await write_header(status, headers, m, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        write_header(http::status status, http::fields const& headers, mode m, boost::system::error_code& ec) override
        {
            for (auto const& f : headers)
            {
                resp_.base().erase(f.name_string());
            }
            resp_.base().result(status);
            for (auto const& f : headers)
            {
                resp_.base().insert(f.name_string(), f.value());
            }

            mode_ = m;
            // chunked 自带分帧；relay 保留上游原有分帧（Content-Length / Transfer-Encoding）。
            if (m == mode::chunked)
            {
                resp_.chunked(true);
            }
            ec = co_await resp_.writer().begin_stream();
        }

        net::awaitable<void>
        write_body(net::const_buffer const& data, bool more) override
        {
            boost::system::error_code ec;
            co_await write_body(data, more, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        write_body(net::const_buffer const& data, bool more, boost::system::error_code& ec) override
        {
            // relay 直通原始字节；chunked 由 body_writer 按 Content-Encoding 自动压缩。
            if (mode_ == mode::relay)
            {
                ec = co_await resp_.writer().write_raw(data, more);
            }
            else
            {
                ec = co_await resp_.writer().write_compressed(data, more);
            }
        }

      private:
        response::impl& resp_;
        mode mode_ = mode::chunked;
    };

} // namespace httplib::server
