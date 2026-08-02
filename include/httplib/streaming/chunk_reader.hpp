#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <string_view>

namespace httplib
{

    class HTTPLIB_API chunk_reader
    {
      public:
        virtual ~chunk_reader() = default;
        virtual net::awaitable<std::string_view> read_chunk() = 0;
        virtual bool is_done() const = 0;
    };

} // namespace httplib
