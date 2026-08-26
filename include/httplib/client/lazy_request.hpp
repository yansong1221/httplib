#pragma once
#include "httplib/client/lazy_response.hpp"
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <optional>
#include <string_view>

namespace httplib::client
{
    class HTTPLIB_API lazy_request
    {
      public:
        virtual ~lazy_request() = default;

        virtual net::awaitable<boost::system::error_code> write_header(http::verb method,
                                                                       std::string_view target,
                                                                       http::fields const& headers,
                                                                       bool relay = true)
            = 0;
        virtual net::awaitable<boost::system::error_code> write_body(net::const_buffer const& data, bool more) = 0;

        // 收尾请求并读取响应头，返回惰性响应（body 未读，可流式读取）。
        virtual net::awaitable<boost::system::result<lazy_response>> read_response() = 0;
    };
} // namespace httplib::client
