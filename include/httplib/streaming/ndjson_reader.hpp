#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>
#include <boost/system/result.hpp>

namespace httplib
{

    class HTTPLIB_API ndjson_reader
    {
      public:
        virtual ~ndjson_reader() = default;

        virtual net::awaitable<boost::system::result<boost::json::value>> read() = 0;

        virtual bool is_done() const = 0;
    };

} // namespace httplib
