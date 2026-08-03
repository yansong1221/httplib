#pragma once
#include "httplib/config.hpp"
#include "httplib/streaming/ndjson_reader.hpp"
#include "httplib/streaming/sse_reader.hpp"
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
    };

    class HTTPLIB_API read_session : public header_read_session
    {
      public:
        virtual ~read_session() = default;
        virtual net::awaitable<boost::system::result<std::size_t>> read_body(net::mutable_buffer const& buffer) = 0;
        virtual http::status result() const = 0;
    };

    class HTTPLIB_API sse_reader
        : public httplib::sse_reader
        , public header_read_session
    {
      public:
        virtual ~sse_reader() = default;
    };

    class HTTPLIB_API ndjson_reader
        : public httplib::ndjson_reader
        , public header_read_session
    {
      public:
        virtual ~ndjson_reader() = default;
    };
} // namespace httplib::client
