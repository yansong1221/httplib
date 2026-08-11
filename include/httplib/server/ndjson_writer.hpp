#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>
#include <string_view>

namespace httplib::server
{

    class HTTPLIB_API ndjson_writer
    {
      public:
        virtual ~ndjson_writer() = default;

        virtual net::awaitable<void> begin() = 0;
        virtual net::awaitable<void> write(boost::json::value const& value, bool more) = 0;
    };

} // namespace httplib::server
