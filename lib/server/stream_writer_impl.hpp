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
                resp_.erase(f.name_string());
            }
            resp_.result(status);
            for (auto const& f : headers)
            {
                resp_.insert(f.name_string(), f.value());
            }

            auto writer_mode = m == mode::relay ? response::impl::body_writer_t::stream_mode::relay
                                                : response::impl::body_writer_t::stream_mode::chunked;
            ec = co_await resp_.writer().begin_stream(writer_mode);
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
            ec = co_await resp_.writer().write_some(data, more);
        }

      private:
        response::impl& resp_;
    };

} // namespace httplib::server
