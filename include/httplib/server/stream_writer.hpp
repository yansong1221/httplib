#pragma once
#include "httplib/config.hpp"
#include "httplib/headers.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>

namespace httplib::server
{

    /// \brief 服务端低层流式写端，由 response 创建。
    /// \note writer 只弱持有创建它的 response：必须在 response 存活期内使用（即
    /// handler 协程内 co_await 完成）。response 销毁后，所有写操作返回
    /// \c net::error::operation_aborted；不支持把 writer 交给 handler 返回后才运行
    /// 的后台任务继续写。
    /// @par 线程安全
    /// 非线程安全。只能在处理该请求的连接 strand 上调用；需要跨线程写入时，调用方
    /// 必须自行 post/dispatch 回该 strand，不能并发调用 write_header / write_body。
    /// 底层 socket I/O 会投递到连接 strand，但这不保护 headers / serializer / mode_。
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

        virtual net::awaitable<void> write_header(httplib::status status, httplib::headers const& headers, mode m) = 0;
        virtual net::awaitable<void> write_header(httplib::status status,
                                                  httplib::headers const& headers,
                                                  mode m,
                                                  boost::system::error_code& ec)
            = 0;

        virtual net::awaitable<void> write_body(net::const_buffer const& data, bool more) = 0;
        virtual net::awaitable<void> write_body(net::const_buffer const& data, bool more, boost::system::error_code& ec)
            = 0;
    };

} // namespace httplib::server
