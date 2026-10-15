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
        /// 传入下载器的 URL 无法解析。
        invalid_url,
        /// 响应体短于声明的长度（Content-Length / Content-Range），重试后仍未补齐。
        truncated_body,
        /// 服务器反复以 304 应答，但对应的缓存体不可用，下载无法完成。
        cache_unavailable,
        /// 连接池没有可用连接，或借出的句柄已失效（过期/已被移走）。
        pool_exhausted,
        /// 对象尚未绑定底层流（如空的 response，或已脱离父 client 的 lazy_request）。
        not_initialized,
        /// 连接池在创建/配置连接时失败。
        connection_failed,
    };

    HTTPLIB_API boost::system::error_code make_error_code(error e);

} // namespace httplib::client
