#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <string_view>

namespace httplib::client {

class HTTPLIB_API relay_session
{
public:
    virtual ~relay_session() = default;

    virtual net::awaitable<boost::system::error_code> write_header(http::verb method,
                                                std::string_view target,
                                                const http::fields& headers)             = 0;
    virtual net::awaitable<boost::system::error_code> write_body(const net::const_buffer& data, bool more)   = 0;
    virtual net::awaitable<boost::system::error_code> read_header()                                          = 0;
    virtual net::awaitable<boost::system::result<std::size_t>> read_body(const net::mutable_buffer& buffer)    = 0;
    virtual http::status result() const                                                 = 0;
    virtual const http::fields& headers() const                                         = 0;
};

} // namespace httplib::client
