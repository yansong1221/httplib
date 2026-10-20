#pragma once
/// \brief JWT 校验器。

#include "httplib/config.hpp"
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string_view>

namespace httplib::jwt
{

    class algorithm;
    class decoded_jwt;

    /// \brief 自定义 claim 检查器谓词。
    ///
    /// 返回 true 表示通过；false、claim 缺失或谓词抛出异常统一报
    /// error::claim_mismatch。
    using claim_check_fn = std::function<bool(boost::json::value const&)>;

    /// \brief 签名验算与 claim 校验。
    ///
    /// `verify()` 不通过时不抛出（除 throwing 形式），原因由 `error` 枚举
    /// 报告；verifier 对象可配置一次后跨调用复用，`verify()` 是 const 的。
    class HTTPLIB_API verifier
    {
      public:
        verifier();
        verifier(verifier const& other);
        verifier& operator=(verifier const& other);
        verifier(verifier&& other) noexcept;
        verifier& operator=(verifier&& other) noexcept;
        ~verifier();

        /// \brief 追加一个允许的算法。重复调用即多算法白名单。
        ///
        /// \note 未配置任何算法的 verifier 校验任何 token 都报
        ///       error::not_initialized（fail-closed）。
        verifier& allow_algorithm(algorithm const& alg);
        /// \brief 校验 iss 与 iss 相等。
        verifier& with_issuer(std::string_view iss);
        /// \brief 校验 sub 与 sub 相等。
        verifier& with_subject(std::string_view sub);
        /// \brief 校验 aud（字符串或数组形态）包含 aud。
        verifier& with_audience(std::string_view aud);
        /// \brief 校验 jti 与 id 相等。
        verifier& with_id(std::string_view id);
        /// \brief 用谓词校验 payload claim。
        ///
        /// \param key claim 键
        /// \param fn  claim 检查器
        verifier& with_claim(std::string_view key, claim_check_fn fn);
        /// \brief 为 exp / nbf / iat 的时间检查附加余量。
        verifier& with_clock_skew(std::chrono::seconds skew);

        /// \brief 校验 token。
        ///
        /// 失败抛出 boost::system::system_error。
        void
        verify(decoded_jwt const& jwt) const
        {
            boost::system::error_code ec;
            verify(jwt, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        /// \brief 同上，失败原因由 `ec` 报告。
        void verify(decoded_jwt const& jwt, boost::system::error_code& ec) const;

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

    /// \brief 构造已允许 alg 的 verifier，便于链式书写。
    inline verifier
    verify(algorithm const& alg)
    {
        verifier v;
        v.allow_algorithm(alg);
        return v;
    }

} // namespace httplib::jwt
