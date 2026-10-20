/// \brief algorithm —— HS 家族：HMAC 直接委托 jwt-cpp，签名返回原始字节
///（与 jwt-cpp 的 builder / verifier 协议一致，无编码往返）。
///
/// jwt-cpp 的 HMAC 引擎在 std::error_code 世界里回报失败，在此翻回公共
/// boost 契约（签名失败族）；std::bad_alloc 一律照常抛出。

#include "httplib/jwt.hpp"
#include "common.hpp"

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
