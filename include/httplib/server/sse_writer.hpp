#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <string_view>

namespace httplib::server
{

    class HTTPLIB_API sse_writer
    {
      public:
        virtual ~sse_writer() = default;

        virtual net::awaitable<boost::system::error_code> begin() = 0;

        virtual net::awaitable<void> send_event(std::string_view data,
                                                std::string_view event,
                                                std::string_view id,
                                                bool more)
            = 0;

        virtual net::awaitable<void> send_retry(std::chrono::milliseconds ms, bool more) = 0;

        virtual net::awaitable<void> send_comment(std::string_view comment, bool more) = 0;
    };

} // namespace httplib::server
