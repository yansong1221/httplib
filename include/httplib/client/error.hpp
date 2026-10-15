#pragma once
#include "httplib/config.hpp"
#include <boost/system/error_code.hpp>

namespace httplib::client
{

    /// \brief 客户端模块错误码。
    enum class error
    {
        /// 服务器返回了非成功状态。具体状态码见 downloader::http_status()。
        http_status = 1,
        /// 跟随重定向的次数超过 config::max_redirects。
        too_many_redirects,
        /// 对端违反了 HTTP 协议（如 206 响应的起始偏移与请求不符）。
        protocol_violation,
    };

    HTTPLIB_API boost::system::error_code make_error_code(error e);

} // namespace httplib::client
