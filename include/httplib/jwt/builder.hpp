#pragma once
/// \brief JWT builder（token 组装与签名）。

#include "httplib/config.hpp"
#include <boost/json/value.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace httplib::jwt
{

    class algorithm;

    /// \brief 逐项设置 header / payload claim 后签名出 token。
    ///
    /// `set_*` 追加写入；重复 `sign()` 安全并允许换算法（见 sign）。
    class HTTPLIB_API builder
    {
      public:
        builder();
        builder(builder const& other);
        builder& operator=(builder const& other);
        builder(builder&& other) noexcept;
        builder& operator=(builder&& other) noexcept;
        ~builder();

        /// \brief header 的 typ。
        builder& set_type(std::string_view type);
        /// \brief header 的 cty。
        builder& set_content_type(std::string_view cty);
        /// \brief header 的 kid。
        builder& set_key_id(std::string_view kid);
        /// \brief 追加自定义 header claim。
        builder& set_header_claim(std::string_view key, boost::json::value value);
        /// \brief payload 的 iss。
        builder& set_issuer(std::string_view iss);
        /// \brief payload 的 sub。
        builder& set_subject(std::string_view sub);
        /// \brief payload 的 aud。
        builder& set_audience(std::string_view aud);
        /// \brief payload 的 jti。
        builder& set_id(std::string_view id);
        /// \brief payload 的 iat。
        builder& set_issued_at(std::chrono::system_clock::time_point t);
        /// \brief payload 的 iat 取当前时间。
        builder& set_issued_now();
        /// \brief payload 的 nbf。
        builder& set_not_before(std::chrono::system_clock::time_point t);
        /// \brief payload 的 exp。
        builder& set_expires_at(std::chrono::system_clock::time_point t);
        /// \brief payload 的 exp 取当前时间 + `d`。
        template <typename Rep, typename Period>
        builder&
        set_expires_in(std::chrono::duration<Rep, Period> d)
        {
            return set_expires_at(std::chrono::system_clock::now() + d);
        }
        /// \brief 追加自定义 payload claim。
        builder& set_payload_claim(std::string_view key, boost::json::value value);

        /// \brief 用 alg 签名生成 token。
        ///
        /// alg 随本次 sign 写入 header；此前 set_key_id / set_content_type /
        /// set_header_claim 写入的 claim 全部保留。失败返回空 result，
        /// 错误码为 error::signature_verification。
        boost::system::result<std::string> sign(algorithm const& alg);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

    /// \brief 构造空 builder，便于链式书写。
    inline builder
    create()
    {
        return builder {};
    }

} // namespace httplib::jwt
