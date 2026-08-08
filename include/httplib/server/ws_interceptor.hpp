#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/fields.hpp>

namespace httplib::server
{
    class HTTPLIB_API ws_interceptor
    {
      public:
        virtual ~ws_interceptor() = default;

        virtual net::awaitable<void>
        on_upstream_request(request& req, http::fields& upstream_headers, std::string const& upstream_url)
        {
            co_return;
        }
        virtual net::awaitable<void>
        on_upstream_send(std::string_view data, bool binary)
        {
            co_return;
        }
        virtual net::awaitable<void>
        on_upstream_recv(std::string_view data, bool binary)
        {
            co_return;
        }
    };
} // namespace httplib::server
