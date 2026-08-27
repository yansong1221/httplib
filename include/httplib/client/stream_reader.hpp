#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace httplib::client
{

    class HTTPLIB_API sse_reader
    {
      public:
        struct sse_event
        {
            std::string id;
            std::string event;
            std::string data;
            std::chrono::milliseconds retry { 0 };
        };

        virtual ~sse_reader() = default;
        virtual net::awaitable<boost::system::result<sse_event>> read_event() = 0;
        virtual bool is_done() const = 0;
    };

    class HTTPLIB_API ndjson_reader
    {
      public:
        virtual ~ndjson_reader() = default;
        virtual net::awaitable<boost::system::result<boost::json::value>> read() = 0;
        virtual bool is_done() const = 0;
    };
} // namespace httplib::client
