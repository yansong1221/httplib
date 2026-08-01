#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string_view>

namespace httplib::client {

class HTTPLIB_API relay_session
{
public:
    virtual ~relay_session() = default;

    virtual net::awaitable<void> write_body(const net::const_buffer& data, bool more) = 0;
    virtual net::awaitable<void> read_header()                                        = 0;
    virtual http::status result() const                                               = 0;
    virtual const http::fields& headers() const                                       = 0;
    virtual net::awaitable<std::size_t> read_body(const net::mutable_buffer& buffer)  = 0;
};

} // namespace httplib::client
