#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/status.hpp>

namespace httplib::server {

class HTTPLIB_API relay_writer
{
public:
    virtual ~relay_writer() = default;

    virtual net::awaitable<void> write_header(http::status status, const http::fields& headers) = 0;
    virtual net::awaitable<void> write_body(const net::const_buffer& data, bool more) = 0;
};

} // namespace httplib::server
