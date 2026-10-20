#include "httplib/config.hpp"
#include "httplib/jwt.hpp" // 公共 API 恒在：失效桩段也按同一签名定义
#ifdef HTTPLIB_ENABLED_JWT
/// \brief verifier —— jwtpp::verifier<system_clock, jtraits> 的 PIMPL。
///
/// 校验全部委托 jwt-cpp 引擎：签名、exp/nbf/iat 时间检查、iss/sub/aud/jti 用
/// 现成具名方法（with_issuer 等），自定义谓词用 with_claim(name, fn)；引擎
/// 负责 claim 提取、校验顺序与第一个错误即短路。本文件仅做错误翻译：
/// 引擎的 token_expired 覆盖 exp/nbf/iat，按 token 实值区分
/// expired / not_yet_valid；claim 失配统一报 claim_mismatch。

#include "impl.hpp"
#include <functional>
#include <utility>

namespace httplib::jwt
{
    verifier::verifier() : impl_(std::make_unique<impl>()) {}
    verifier::verifier(verifier const& other) : impl_(std::make_unique<impl>(*other.impl_)) {}
    verifier::verifier(verifier&& other) noexcept = default;
    verifier::~verifier() = default;
    verifier&
    verifier::operator=(verifier const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(*other.impl_);
        }
        return *this;
    }
    verifier& verifier::operator=(verifier&& other) noexcept = default;

    verifier&
    verifier::allow_algorithm(algorithm const& alg)
    {
        // 追加而非覆盖：多算法并行的轮换场景（jwt-cpp 本就是按名入 map）。
        impl_->engine.allow_algorithm(detail::algo_adapter { alg.clone() });
        impl_->allowed += 1;
        return *this;
    }

    verifier&
    verifier::with_issuer(std::string_view iss)
    {
        // 直接构造引擎原生 string_type（boost::json::string），跳过 std::string
        // 中转拷贝 —— with_subject / with_audience / with_id 同理。
        impl_->engine.with_issuer(jtraits::string_type { iss });
        return *this;
    }

    verifier&
    verifier::with_subject(std::string_view sub)
    {
        impl_->engine.with_subject(jtraits::string_type { sub });
        return *this;
    }

    verifier&
    verifier::with_audience(std::string_view aud)
    {
        // jwt-cpp 的字符串重载：token 的 aud（字符串或字符串数组）必须包含 aud
        //（is_subset_claim 语义）。
        impl_->engine.with_audience(jtraits::string_type { aud });
        return *this;
    }

    verifier&
    verifier::with_id(std::string_view id)
    {
        impl_->engine.with_id(jtraits::string_type { id });
        return *this;
    }

    verifier&
    verifier::with_claim(std::string_view key, claim_check_fn fn)
    {
        // 本库谓词形态 bool(json const&) → jwt-cpp 检查器形态：(ctx, ec)。
        // claim 缺失 / 形态异常 / 谓词不过统一置 claim_mismatch（std 类别，
        // 由 verify() 结束时翻回 boost 侧）。
        auto k = jtraits::string_type { key };
        impl_->engine.with_claim(k,
            [k, fn = std::move(fn)](jwtpp::verify_ops::verify_context<jtraits> const& ctx, std::error_code& ec)
            {
                try
                {
                    if (!ctx.jwt.has_payload_claim(k) || !fn(ctx.jwt.get_payload_claim(k).to_json()))
                    {
                        ec = detail::std_of(error::claim_mismatch);
                    }
                }
                catch (std::exception const&)
                {
                    ec = detail::std_of(error::claim_mismatch);
                }
            });
        return *this;
    }

    verifier&
    verifier::with_clock_skew(std::chrono::seconds skew)
    {
        // 与 jwt-cpp 一致：leeway 作用于 exp / nbf / iat 全部时间原语。
        impl_->engine.leeway(static_cast<size_t>(skew.count()));
        impl_->skew = skew;
        return *this;
    }

    void
    verifier::verify(decoded_jwt const& jwt, boost::system::error_code& ec) const
    {
        // fail-closed：没有白名单的 verifier 不放行任何 token。
        if (impl_->allowed == 0)
        {
            ec = make_error_code(error::not_initialized);
            return;
        }

        auto& inner = jwt.impl_->inner;
        if (!inner)
        {
            // 解析失败的 token 无从校验 —— 原样回报构造期记下的错误。
            ec = jwt.impl_->error_;
            return;
        }

        std::error_code jec;
        impl_->engine.verify(*inner, jec);
        if (!jec)
        {
            ec.clear();
            return;
        }

        // ---- 错误翻译：公共契约的精确错误码 ----
        auto const& cat = jec.category();
        if (cat == detail::jwt_std_category())
        {
            // 本库检查器（自定义谓词）写入的错误码原样翻回 boost 侧
            ec = boost::system::error_code { jec.value(), detail::jwt_boost_category() };
            return;
        }
        if (cat == jwtpp::error::signature_verification_error_category() ||
            cat == jwtpp::error::signature_generation_error_category())
        {
            ec = make_error_code(error::signature_verification);
            return;
        }
        if (cat == jwtpp::error::token_verification_error_category())
        {
            switch (static_cast<jwtpp::error::token_verification_error>(jec.value()))
            {
                case jwtpp::error::token_verification_error::wrong_algorithm:
                    // 引擎白名单不含 token 声明的 alg
                    ec = make_error_code(error::algorithm_mismatch);
                    return;
                case jwtpp::error::token_verification_error::token_expired:
                    // jwt-cpp 把 exp / nbf / iat 都折成 token_expired；公共契约
                    // 区分"尚未生效"与"已过期"（时间状态）。
                    try
                    {
                        auto const now = std::chrono::system_clock::now();
                        if ((inner->has_not_before() && now < inner->get_not_before() - impl_->skew) ||
                            (inner->has_issued_at() && now < inner->get_issued_at() - impl_->skew))
                        {
                            ec = make_error_code(error::not_yet_valid);
                        }
                        else
                        {
                            ec = make_error_code(error::expired);
                        }
                    }
                    catch (std::exception const&)
                    {
                        // 时间 claim 形态异常无法判别 —— 归为过期
                        ec = make_error_code(error::expired);
                    }
                    return;
                case jwtpp::error::token_verification_error::claim_value_missmatch:
                    // iss/sub/aud/jti/自定义谓词的失配统一码（与 jwt-cpp 同粒度）
                    ec = make_error_code(error::claim_mismatch);
                    return;
                default:
                    // 未列举的 token 校验失败 —— 防御性归类
                    ec = make_error_code(error::claim_mismatch);
                    return;
            }
        }
        // 未识别的错误类别 —— 归入签名失败族
        ec = make_error_code(error::signature_verification);
    }

} // namespace httplib::jwt

#else // !HTTPLIB_ENABLED_JWT —— 运行期失效桩（同 use_ssl 形态）

#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
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

        [[noreturn]] inline void
        unavailable_noexcept()
        {
            std::abort();
        }
    } // namespace

    class verifier::impl
    {
    };

    verifier::verifier() { unavailable(); }
    verifier::verifier(verifier const&) : impl_(nullptr) { unavailable(); }
    verifier&
    verifier::operator=(verifier const&)
    {
        unavailable();
    }
    verifier::verifier(verifier&& other) noexcept : impl_(std::move(other.impl_))
    {
        unavailable_noexcept();
    }
    verifier&
    verifier::operator=(verifier&&) noexcept
    {
        unavailable_noexcept();
    }
    verifier::~verifier() = default;

    verifier&
    verifier::allow_algorithm(algorithm const& alg)
    {
        (void)alg;
        unavailable();
    }
    verifier&
    verifier::with_issuer(std::string_view)
    {
        unavailable();
    }
    verifier&
    verifier::with_subject(std::string_view)
    {
        unavailable();
    }
    verifier&
    verifier::with_audience(std::string_view)
    {
        unavailable();
    }
    verifier&
    verifier::with_id(std::string_view)
    {
        unavailable();
    }
    verifier&
    verifier::with_claim(std::string_view, claim_check_fn)
    {
        unavailable();
    }
    verifier&
    verifier::with_clock_skew(std::chrono::seconds)
    {
        unavailable();
    }
    void
    verifier::verify(decoded_jwt const&, boost::system::error_code& ec) const
    {
        ec = boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported);
    }

} // namespace httplib::jwt
#endif // HTTPLIB_ENABLED_JWT
