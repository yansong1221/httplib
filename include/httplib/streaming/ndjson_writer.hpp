#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <string_view>

namespace httplib {

class ndjson_writer
{
public:
    virtual ~ndjson_writer() = default;

    virtual net::awaitable<void> write(const boost::json::value& value) = 0;

    virtual net::awaitable<void> close() = 0;
};

} // namespace httplib
