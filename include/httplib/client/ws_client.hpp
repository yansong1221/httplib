#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/http/fields.hpp>
#include <string_view>

namespace httplib::client {
class ws_client
{
public:
    explicit ws_client(net::io_context& ex, std::string_view host, uint16_t port, bool ssl = false);
    explicit ws_client(const net::any_io_executor& ex,
                       std::string_view host,
                       uint16_t port,
                       bool ssl = false);
    ~ws_client();

public:
    net::awaitable<boost::system::error_code> async_connect(std::string_view path,
                                                            const http::fields& headers = {});

    bool got_binary() const noexcept;
    bool got_text() const noexcept;

private:
    class impl;
    std::unique_ptr<impl> impl_;
};
} // namespace httplib::client