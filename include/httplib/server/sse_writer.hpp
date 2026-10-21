#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <string_view>

namespace httplib::server
{

    /// \brief 服务端 SSE 写端，由 response 创建。
    /// \note 绑定创建它的 response：必须在 response 存活期内使用（即 handler 协程内
    /// co_await 完成）。response 销毁后所有写操作返回 \c net::error::operation_aborted。
    /// @par 线程安全
    /// 非线程安全。只能在处理该请求的连接 strand 上调用；需要跨线程推送事件时，
    /// 调用方必须自行 post/dispatch 回该 strand，不能并发调用 send_event / send_retry /
    /// send_comment。
    class HTTPLIB_API sse_writer
    {
      public:
        virtual ~sse_writer() = default;

        virtual net::awaitable<void> begin() = 0;
        virtual net::awaitable<void> begin(boost::system::error_code& ec) = 0;

        virtual net::awaitable<void> send_event(std::string_view data,
                                                std::string_view event,
                                                std::string_view id,
                                                bool more)
            = 0;
        virtual net::awaitable<void> send_event(std::string_view data,
                                                std::string_view event,
                                                std::string_view id,
                                                bool more,
                                                boost::system::error_code& ec)
            = 0;

        virtual net::awaitable<void> send_retry(std::chrono::milliseconds ms, bool more) = 0;
        virtual net::awaitable<void> send_retry(std::chrono::milliseconds ms, bool more, boost::system::error_code& ec)
            = 0;
        virtual net::awaitable<void> send_comment(std::string_view comment, bool more) = 0;
        virtual net::awaitable<void> send_comment(std::string_view comment, bool more, boost::system::error_code& ec)
            = 0;
    };

} // namespace httplib::server
