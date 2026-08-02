#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <memory>
#include <string_view>

namespace httplib::client
{
    class HTTPLIB_API read_session
    {
      public:
        virtual ~read_session() = default;

        virtual net::awaitable<boost::system::error_code> read_header() = 0;
        virtual net::awaitable<boost::system::result<std::size_t>> read_body(net::mutable_buffer const& buffer) = 0;
        virtual http::status result() const = 0;
        virtual http::fields const& headers() const = 0;
    };
} // namespace httplib::client
