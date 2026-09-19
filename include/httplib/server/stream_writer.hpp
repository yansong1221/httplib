#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/system/error_code.hpp>

namespace httplib::server
{

    class HTTPLIB_API stream_writer
    {
      public:
        virtual ~stream_writer() = default;

        enum class mode
        {
            /// 透传上游原始字节，不做二次压缩（反向代理）。
            relay,
            /// 自身流式输出（chunked，可压缩）。
            chunked,
        };

        virtual net::awaitable<void> write_header(http::status status, http::fields const& headers, mode m) = 0;
        virtual net::awaitable<void> write_header(http::status status,
                                                  http::fields const& headers,
                                                  mode m,
                                                  boost::system::error_code& ec)
            = 0;

        virtual net::awaitable<void> write_body(net::const_buffer const& data, bool more) = 0;
        virtual net::awaitable<void> write_body(net::const_buffer const& data, bool more, boost::system::error_code& ec)
            = 0;
    };

} // namespace httplib::server
