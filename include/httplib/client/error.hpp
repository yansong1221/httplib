#pragma once
#include "httplib/config.hpp"
#include <boost/system/error_code.hpp>
#include <type_traits>

namespace httplib::client
{

    /// \brief 客户端模块错误码。
    enum class error
    {
        /// 服务器返回了非成功状态。具体状态码见 downloader::http_status() 或
        /// proxy_client::http_status()。
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
        /// 在 wait_timeout 内没有从连接池借到可用连接（含 fail-fast）。
        acquire_timeout,
        /// 句柄不持有连接（默认构造、被移动、或已 release）。
        invalid_handle,
        /// 对象尚未绑定底层流（如空的 response，或已脱离父 client 的 lazy_request）。
        not_initialized,
        /// 该 client 上已有进行中的请求，或上一条响应体尚未读完。
        already_in_use,
    };

    HTTPLIB_API boost::system::error_code make_error_code(error e);

} // namespace httplib::client

namespace boost::system
{
    /// 让 `error` 可直接隐式转换为 `boost::system::error_code`
    /// （如 `boost::system::error_code ec = httplib::client::error::http_status;`），
    /// 与 httplib::compress::error 的注册方式保持一致。
    template <>
    struct is_error_code_enum<httplib::client::error> : std::true_type
    {
    };
} // namespace boost::system
