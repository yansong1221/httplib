#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>
#include <string_view>

namespace httplib::server
{

    /// \brief 服务端 NDJSON 写端，由 response 创建。
    /// \note 绑定创建它的 response：必须在 response 存活期内使用（即 handler 协程内
    /// co_await 完成）。response 销毁后所有写操作返回 \c net::error::operation_aborted。
    /// @par 线程安全
    /// 非线程安全。只能在处理该请求的连接 strand 上调用；需要跨线程写入时，调用方
    /// 必须自行 post/dispatch 回该 strand，不能并发调用 begin / write。
    class HTTPLIB_API ndjson_writer
    {
      public:
        virtual ~ndjson_writer() = default;

        virtual net::awaitable<void> begin() = 0;
        virtual net::awaitable<void> begin(boost::system::error_code& ec) = 0;

        virtual net::awaitable<void> write(boost::json::value const& value, bool more) = 0;
        virtual net::awaitable<void> write(boost::json::value const& value, bool more, boost::system::error_code& ec)
            = 0;
    };

} // namespace httplib::server
