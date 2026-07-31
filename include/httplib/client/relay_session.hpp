#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string_view>

namespace httplib::client {

class HTTPLIB_API relay_session
{
public:
    relay_session()                                 = default;
    virtual ~relay_session()                        = default;
    relay_session(const relay_session&)             = delete;
    relay_session& operator=(const relay_session&)  = delete;

    relay_session(relay_session&&)                  = default;
    relay_session& operator=(relay_session&&)       = default;

    virtual net::awaitable<void> write_body(std::string_view data)  = 0;
    virtual net::awaitable<void> close_body()                       = 0;

    virtual net::awaitable<void> read_header()                         = 0;
    virtual http::status result() const                             = 0;
    virtual const http::fields& headers() const                     = 0;

    virtual net::awaitable<std::string_view> read_some_body()       = 0;
    virtual bool is_body_done() const                               = 0;
};

} // namespace httplib::client
