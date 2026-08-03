#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <optional>
#include <string_view>

namespace httplib::client
{
    class HTTPLIB_API write_session
    {
      public:
        virtual ~write_session() = default;

        virtual net::awaitable<boost::system::error_code> write_header(http::verb method,
                                                                       std::string_view target,
                                                                       http::fields const& headers,
                                                                       bool relay = true)
            = 0;
        virtual net::awaitable<boost::system::error_code> write_body(net::const_buffer const& data, bool more) = 0;
    };
} // namespace httplib::client
