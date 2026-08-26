#pragma once
#include "httplib/client/request.hpp"
#include <boost/beast/http/message.hpp>
#include <memory>

namespace httplib::client
{
    class request::impl : public http::request<body::any_body>
    {
      public:
        impl(http::verb method, std::string_view target, unsigned version)
            : http::request<body::any_body>(method, target, version)
        {
        }
    };
} // namespace httplib::client
