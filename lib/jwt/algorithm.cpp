#include "httplib/config.hpp"
#include "httplib/jwt.hpp" // 公共 API 恒在：失效桩段也按同一签名定义
#ifdef HTTPLIB_ENABLED_JWT
/// \brief algorithm —— HS 家族：HMAC 直接委托 jwt-cpp，签名返回原始字节
///（与 jwt-cpp 的 builder / verifier 协议一致，无编码往返）。
///
/// jwt-cpp 的 HMAC 引擎在 std::error_code 世界里回报失败，在此翻回公共
/// boost 契约（签名失败族）；std::bad_alloc 一律照常抛出。

#include "engine.hpp"

namespace httplib::jwt
{
    std::string
    hs256::sign(std::string_view data, boost::system::error_code& ec) const
    {
        std::error_code sec;
        auto sig = jwtpp::algorithm::hs256 { secret_ }.sign(std::string { data }, sec);
        if (sec)
        {
            ec = make_error_code(error::signature_verification);
            return {};
        }
        ec.clear();
        return sig;
    }

    std::string
    hs384::sign(std::string_view data, boost::system::error_code& ec) const
    {
        std::error_code sec;
        auto sig = jwtpp::algorithm::hs384 { secret_ }.sign(std::string { data }, sec);
        if (sec)
        {
            ec = make_error_code(error::signature_verification);
            return {};
        }
        ec.clear();
        return sig;
    }

    std::string
    hs512::sign(std::string_view data, boost::system::error_code& ec) const
    {
        std::error_code sec;
        auto sig = jwtpp::algorithm::hs512 { secret_ }.sign(std::string { data }, sec);
        if (sec)
        {
            ec = make_error_code(error::signature_verification);
            return {};
        }
        ec.clear();
        return sig;
    }

} // namespace httplib::jwt

#else // !HTTPLIB_ENABLED_JWT —— 运行期失效桩：公共头恒定声明本模块 API，
      // 未启用构建链接本文件，所有入口以 protocol_not_supported 快速失败
      // （noexcept 入口用 std::abort；与 server_impl.cpp 的 use_ssl 同形态）。

#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::jwt
{
    namespace
    {
        [[noreturn]] inline void
        unavailable()
        {
            throw boost::system::system_error(
                boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported));
        }
    } // namespace

    // HS 家族：name / clone / 构造均为头文件内联，无链接依赖；仅 sign 为
    // out-of-line 定义（vtable 弱符号引用它），此处兜底。
    std::string
    hs256::sign(std::string_view data, boost::system::error_code& ec) const
    {
        (void)data;
        ec = boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported);
        return {};
    }

    std::string
    hs384::sign(std::string_view data, boost::system::error_code& ec) const
    {
        (void)data;
        ec = boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported);
        return {};
    }

    std::string
    hs512::sign(std::string_view data, boost::system::error_code& ec) const
    {
        (void)data;
        ec = boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported);
        return {};
    }

} // namespace httplib::jwt
#endif // HTTPLIB_ENABLED_JWT
