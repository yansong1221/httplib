#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <string>

namespace httplib
{

    struct sse_event
    {
        std::string id;
        std::string event;
        std::string data;
        std::chrono::milliseconds retry { 0 };
    };

    class HTTPLIB_API sse_reader
    {
      public:
        virtual ~sse_reader() = default;
        virtual net::awaitable<boost::system::result<sse_event>> read_event() = 0;
        virtual bool is_done() const = 0;
    };

} // namespace httplib
