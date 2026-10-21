#pragma once
#include "httplib/client/response.hpp"
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <optional>
#include <string_view>

namespace httplib::client
{
    /// \brief 绑定到某个 client 的增量请求写端。
    /// \details 每次 write_header 成功时会在所属 client 上登记，响应头到达后自动注销，
    /// 因此“已创建但空闲”或“上一次交换已完成”的 writer 不会阻止连接池复用。
    /// 同一 client 同一时刻只允许一个请求在途：若另一个 writer 正在发送，或上一条响应体
    /// 尚未读完，write_header 返回 \c error::already_in_use。
    /// \note writer 的使用窗口不得超出 client（或连接池 handle）的借出窗口：归还 handle
    /// 后不应继续使用先前取得的 writer。
    class HTTPLIB_API lazy_request
    {
      public:
        virtual ~lazy_request() = default;

        enum class mode
        {
            relay,
            chunked,
        };

        virtual net::awaitable<void> write_header(httplib::method method,
                                                  std::string_view target,
                                                  httplib::headers const& headers,
                                                  mode m)
            = 0;
        virtual net::awaitable<void> write_header(httplib::method method,
                                                  std::string_view target,
                                                  httplib::headers const& headers,
                                                  mode m,
                                                  boost::system::error_code& ec)
            = 0;

        virtual net::awaitable<void> write_body(net::const_buffer const& data, bool more) = 0;
        virtual net::awaitable<void> write_body(net::const_buffer const& data, bool more, boost::system::error_code& ec)
            = 0;

        // 收尾请求并读取响应头，返回惰性响应（body 未读，可流式读取）。
        virtual net::awaitable<boost::system::result<response>> read_response_lazy() = 0;

        // 收尾请求并读取完整响应（body 已全部读入）。
        virtual net::awaitable<boost::system::result<response>> read_response() = 0;
    };
} // namespace httplib::client
