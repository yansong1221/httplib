#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/client/response.hpp"
#include <boost/beast/http/message.hpp>
#include <memory>

namespace httplib::client
{
    class response::impl
    {
      public:
        impl() = default;
        explicit impl(http::response<body::any_body>&& msg) : msg_(std::move(msg)) {}

        static response
        make(http::response<body::any_body>&& msg)
        {
            return response(std::make_shared<impl>(std::move(msg)));
        }

        http::response<body::any_body> msg_;
    };
} // namespace httplib::client
