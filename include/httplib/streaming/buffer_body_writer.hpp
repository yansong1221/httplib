#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <string_view>

namespace httplib::streaming
{

    class HTTPLIB_API buffer_body_writer
    {
      public:
        virtual ~buffer_body_writer() = default;
        virtual net::awaitable<void> write(net::const_buffer const& data, bool more) = 0;
    };

} // namespace httplib::streaming
