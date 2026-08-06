#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/status.hpp>

namespace httplib::server
{
    class HTTPLIB_API proxy_interceptor
    {
      public:
        virtual ~proxy_interceptor() = default;

        virtual net::awaitable<void> on_upstream_request(request& req,
                                                         http::fields& upstream_headers,
                                                         const std::string& upstream_url)
        {
            co_return;
        }
        virtual net::awaitable<void> on_upstream_request_body(net::const_buffer buffer, bool more)
        {
            co_return;
        }
        virtual net::awaitable<void> on_upstream_response(request& req,
                                                          http::status upstream_status,
                                                          const http::fields& upstream_resp_headers)
        {
            co_return;
        }
        virtual net::awaitable<void> on_upstream_response_body(net::const_buffer buffer, bool more)
        {
            co_return;
        }
    };
} // namespace httplib::server
