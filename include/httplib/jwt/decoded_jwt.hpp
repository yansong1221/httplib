#pragma once
/// \brief 解析后的 JWT token。
///
/// base64URL 与 JSON 解析完全委托 jwt-cpp；本类不抛出解析异常，
/// 失败信息经 get_error() / decode() 报告。

#include "httplib/config.hpp"
#include <boost/json/value.hpp>
#include <boost/json/value_to.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <concepts>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace httplib::jwt
{

    class builder;
    class algorithm;
    class verifier;

    class HTTPLIB_API decoded_jwt
    {
      public:
        /// \brief 解析三段式 token。
        ///
        /// 除 std::bad_alloc 外不抛出。token 不是合法 JWT、缺 alg 或 alg
        /// 不是字符串时保持空对象，原因记录在 get_error()。
        ///
        /// \param token token 文本
        decoded_jwt(std::string_view token);
        decoded_jwt(decoded_jwt const& other);
        decoded_jwt& operator=(decoded_jwt const& other);
        decoded_jwt(decoded_jwt&& other) noexcept;
        decoded_jwt& operator=(decoded_jwt&& other) noexcept;
        ~decoded_jwt();

        /// \brief 解析失败原因；成功时为空。
        boost::system::error_code get_error() const;

        /// \brief payload 部分的 JSON 镜像。
        boost::json::value get_payload() const;
        /// \brief header 部分的 JSON 镜像。
        boost::json::value get_header() const;
        /// \brief 原始 token 文本。
        std::string_view get_token() const;
        /// \brief 签名部分的 base64url 文本段（验签比对时解码为原始字节）。
        std::string_view get_signature() const;
        /// \brief header 中的 alg 值；解析失败时为空串。
        std::string_view get_algorithm() const;

        /// \brief 标准注册 claim 是否存在。
        bool has_issuer() const;
        bool has_subject() const;
        bool has_audience() const;
        bool has_id() const;
        bool has_issued_at() const;
        bool has_not_before() const;
        bool has_expires_at() const;
        bool has_type() const;

        /// \brief 标准注册 claim 的值。
        ///
        /// aud 为多值时以逗号连接返回。claim 缺失或类型不符时抛出
        /// std::out_of_range / std::bad_cast（与 jwt-cpp 行为一致）。
        std::string get_issuer() const;
        std::string get_subject() const;
        std::string get_audience() const;
        std::string get_id() const;
        std::string get_type() const;
        std::chrono::system_clock::time_point get_issued_at() const;
        std::chrono::system_clock::time_point get_not_before() const;
        std::chrono::system_clock::time_point get_expires_at() const;

        /// \brief 按 key 取 payload claim 并解析为 T。
        ///
        /// \throw std::out_of_range claim 缺失
        /// \throw 转型失败时抛出 boost::json 的对应异常
        template <typename T>
        auto
        get_payload_claim(std::string_view key) const
        {
            return boost::json::value_to<T>(get_payload().at(key));
        }

        /// \brief 仅用 alg 比对签名，不校验 claims（claim 校验是 verifier 的职责）。
        ///
        /// \param alg 与签发者一致的算法
        /// \return 签名一致返回 true
        bool verify(algorithm const& alg) const;
        /// \brief 同上，失败原因经 `ec` 报告。
        bool verify(algorithm const& alg, boost::system::error_code& ec) const;

      private:
        friend class verifier;

        class impl;
        std::unique_ptr<impl> impl_;
    };

    /// \brief 解析 token；失败时返回携带 error::invalid_token 的空 result。
    HTTPLIB_API boost::system::result<decoded_jwt> decode(std::string_view token);
    template <std::convertible_to<std::string_view> S>
    boost::system::result<decoded_jwt>
    decode(S&& token)
    {
        return decode(std::string_view(std::forward<S>(token)));
    }

} // namespace httplib::jwt
