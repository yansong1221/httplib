#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <memory>
#include <string_view>

namespace httplib::client
{
    class HTTPLIB_API header_read_session
    {
      public:
        virtual ~header_read_session() = default;
        virtual bool is_header_done() const = 0;
        virtual net::awaitable<boost::system::error_code> read_header() = 0;
        virtual http::fields const& headers() const = 0;
        virtual http::status result() const = 0;
    };

    class HTTPLIB_API read_session : public header_read_session
    {
      public:
        virtual ~read_session() = default;
        virtual net::awaitable<boost::system::result<std::size_t>> read_body(net::mutable_buffer const& buffer) = 0;
        virtual bool is_body_done() const = 0;
    };

    class HTTPLIB_API sse_reader : public header_read_session
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

    class HTTPLIB_API ndjson_reader : public header_read_session
    {
      public:
        virtual ~ndjson_reader() = default;
        virtual net::awaitable<boost::system::result<boost::json::value>> read() = 0;
        virtual bool is_done() const = 0;
    };
} // namespace httplib::client
