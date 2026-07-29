#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <string_view>

namespace httplib {

class chunk_writer;

using chunked_write_handler_type = std::function<net::awaitable<void>(chunk_writer&)>;

class HTTPLIB_API chunk_writer
{
public:
    virtual ~chunk_writer()                       = default;
    virtual net::awaitable<void> write_chunk(std::string_view data) = 0;
    virtual net::awaitable<void> close()          = 0;
};

} // namespace httplib
