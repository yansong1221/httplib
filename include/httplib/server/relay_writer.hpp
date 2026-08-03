#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/system/error_code.hpp>

namespace httplib::server
{

    class HTTPLIB_API relay_writer
    {
      public:
        virtual ~relay_writer() = default;

        virtual net::awaitable<boost::system::error_code> write_header(http::status status,
                                                                       http::fields const& headers,
                                                                       bool relay = true)
            = 0;
        virtual net::awaitable<boost::system::error_code> write_body(net::const_buffer const& data, bool more) = 0;
    };

} // namespace httplib::server
