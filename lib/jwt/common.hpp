#pragma once
/// \brief JWT 实现内部共享件（公共头不含本文件）。
///
/// 实现侧不做任何 JWT 逻辑的二次实现：
///   * base64url 编解码 / JSON（反）序列化       → jwt-cpp base.h + traits
///   * token 解析（三段式、claims 提取）         → jwtpp::decoded_jwt<traits>
///   * token 组装与签名（alg claim 写入）        → jwtpp::builder<system_clock, traits>
///   * HMAC 与常量时间签名比较                   → jwtpp::algorithm::hs*
///   * 签名验算与 exp/nbf/iat 时间检查           → jwtpp::verifier<system_clock, traits>
///
/// 本目录文件只做两种 glue：
///   1. PIMPL —— 公共 API 不泄漏 jwt-cpp 类型（与 Beast 同规）；
///   2. 错误翻译 —— 引擎信号（token_expired 覆盖 exp/nbf/iat、claim 失配为
///      泛型码）翻回公共 error 枚举（粒度一致，见 error.hpp / verifier.cpp）。

#include "httplib/jwt.hpp"
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/boost-json/traits.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace httplib::jwt
{
    /// 全局 ::jwt 被 httplib::jwt 遮蔽，实现内一律经 jwtpp 别名访问。
    namespace jwtpp = ::jwt;
}

namespace httplib::jwt::detail
{
    using jtraits = jwtpp::traits::boost_json;
    using jdecoded = jwtpp::decoded_jwt<jtraits>;
    using jbuilder = jwtpp::builder<std::chrono::system_clock, jtraits>;
    using jverifier = jwtpp::verifier<std::chrono::system_clock, jtraits>;
    using jclaim = jwtpp::basic_claim<jtraits>;

    std::string jwt_error_message(int ev) noexcept;
    boost::system::error_category const& jwt_boost_category() noexcept;
    std::error_category const& jwt_std_category() noexcept;
    std::error_code std_of(error e) noexcept;

    /// base64url 解码（带 padding 归一化，与 jwt-cpp decoded_jwt 的解码路径一致）。
    inline std::string
    decode_base64url(std::string const& text)
    {
        return jwtpp::base::decode<jwtpp::alphabet::base64url>(jwtpp::base::pad<jwtpp::alphabet::base64url>(text));
    }

    /// 本库抽象 algorithm 与 jwt-cpp 引擎之间的转发桥：公共原语与引擎的
    /// Algorithm concept 逐字同构（sign 返回原始字节），无编码换算。
    /// 错误通道横跨两个世界 —— 引擎在 std::error_code 运作，公共原语是
    /// boost::system::error_code；boost 侧错误码经 boost 自带的 std 兼容
    /// 包装原样穿透（值与 message 保持一致）。
    struct algo_adapter
    {
        std::shared_ptr<httplib::jwt::algorithm const> alg;

        std::string
        name() const
        {
            return alg->name();
        }

        std::string
        sign(std::string const& data, std::error_code& ec) const
        {
            boost::system::error_code bec;
            auto const out = alg->sign(data, bec);
            if (bec)
            {
                ec = std::error_code(bec);
                return {};
            }
            ec.clear();
            return out;
        }

        void
        verify(std::string const& data, std::string const& signature, std::error_code& ec) const
        {
            // 与 jwt-cpp hmac::verify 相同：重算签名后做常量时间全字节比较
            //（不因首个差异位置提前返回）。
            boost::system::error_code bec;
            auto const expected = alg->sign(data, bec);
            if (!bec)
            {
                bool matched = expected.size() == signature.size();
                for (size_t i = 0; i < (std::min)(expected.size(), signature.size()); i++)
                {
                    if (expected[i] != signature[i]) matched = false;
                }
                if (matched)
                {
                    return;
                }
            }
            ec = jwtpp::error::signature_verification_error::invalid_signature;
        }
    };
} // namespace httplib::jwt::detail
