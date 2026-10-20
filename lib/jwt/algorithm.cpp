// algorithm —— HS 家族：HMAC 直接委托 jwt-cpp，签名返回原始字节（与 jwt-cpp
// 的 builder/verifier 协议一致，无任何编码往返）；失败经 ec 报告并返回空串。

#include "httplib/jwt.hpp"
#include "common.hpp"

namespace httplib::jwt
{
    std::string
    hs256::sign(std::string_view data, std::error_code& ec) const
    {
        // jwtpp 的 hmac sign/verify 起手即 ec.clear()，原始字节直通。
        return jwtpp::algorithm::hs256 { secret_ }.sign(std::string { data }, ec);
    }

    std::string
    hs384::sign(std::string_view data, std::error_code& ec) const
    {
        return jwtpp::algorithm::hs384 { secret_ }.sign(std::string { data }, ec);
    }

    std::string
    hs512::sign(std::string_view data, std::error_code& ec) const
    {
        return jwtpp::algorithm::hs512 { secret_ }.sign(std::string { data }, ec);
    }

} // namespace httplib::jwt
