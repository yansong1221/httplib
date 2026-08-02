#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>

namespace httplib
{

    class ndjson_reader
    {
      public:
        virtual ~ndjson_reader() = default;

        virtual net::awaitable<boost::json::value> read() = 0;

        virtual bool is_done() const = 0;
    };

} // namespace httplib
