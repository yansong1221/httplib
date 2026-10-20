#pragma once
/// \brief JWT 模块公共错误码与 boost::system 错误注册。
///
/// 错误码粒度与 jwt-cpp 对齐：claim 失配统一为 claim_mismatch；仅时间状态
/// 区分 expired / not_yet_valid。

#include "httplib/config.hpp"
#include <boost/system/error_code.hpp>
#include <type_traits>

namespace httplib::jwt
{

    /// \brief 错误码。
    enum class error
    {
        /// 重算签名与 token 签名段不一致，或签名段不是合法 base64url。
        signature_verification = 1,
        /// 不是三段式 token、base64 段解不出 JSON、缺 alg 或 alg 不是字符串。
        invalid_token,
        /// 已过 exp。
        expired,
        /// 未到 nbf 或 iat 在未来。
        not_yet_valid,
        /// token 声明的算法不在允许列表。
        algorithm_mismatch,
        /// verifier 未配置任何允许算法，拒绝校验（fail-closed）。
        not_initialized,
        /// claim 校验失败（iss / sub / aud / jti / 自定义谓词同粒度）。
        claim_mismatch,
    };

    /// \brief 构造 `httplib.jwt` 类别的 error_code。
    HTTPLIB_API boost::system::error_code make_error_code(error e);

} // namespace httplib::jwt

namespace boost::system
{
    /// \brief error 的 is_error_code_enum 注册，
    ///        使 `boost::system::error_code ec = httplib::jwt::error::expired;`
    ///        直接成立（与 httplib::client::error 一致）。
    template <>
    struct is_error_code_enum<httplib::jwt::error> : std::true_type
    {
    };
} // namespace boost::system
