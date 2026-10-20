#pragma once
/// \brief 抽象签名算法与 HS 家族。
///
/// `sign()` 原语与 jwt-cpp 的 Algorithm concept 一致：返回原始字节，
/// base64url 编码由引擎（builder / verifier）负责。错误通道为公共契约的
/// boost::system::error_code，自定义算法用 make_error_code 报告失败。

#include "httplib/config.hpp"
#include <boost/system/error_code.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::jwt
{

    /// \brief 抽象签名算法。自定义算法继承本类，把实例传给 builder / verifier。
    class HTTPLIB_API algorithm
    {
      public:
        virtual ~algorithm() = default;

        /// \brief JWS 算法名（如 "HS256"）。
        virtual std::string name() const = 0;

        /// \brief 计算 `data` 的签名。
        ///
        /// \return 签名原始字节；失败返回空串并在 `ec` 报告原因
        ///        （如 make_error_code(error::signature_verification)）。
        virtual std::string sign(std::string_view data, boost::system::error_code& ec) const = 0;

        /// \brief 复制自身（builder / verifier 内部按值持有）。
        virtual std::shared_ptr<algorithm> clone() const = 0;
    };

    /// \brief HMAC-SHA256。
    struct HTTPLIB_API hs256 : algorithm
    {
        explicit hs256(std::string_view secret) : secret_(secret) {}
        std::string
        name() const override
        {
            return "HS256";
        }
        std::string sign(std::string_view data, boost::system::error_code& ec) const override;
        std::shared_ptr<algorithm>
        clone() const override
        {
            return std::make_shared<hs256>(secret_);
        }

      private:
        std::string secret_;
    };

    /// \brief HMAC-SHA384。
    struct HTTPLIB_API hs384 : algorithm
    {
        explicit hs384(std::string_view secret) : secret_(secret) {}
        std::string
        name() const override
        {
            return "HS384";
        }
        std::string sign(std::string_view data, boost::system::error_code& ec) const override;
        std::shared_ptr<algorithm>
        clone() const override
        {
            return std::make_shared<hs384>(secret_);
        }

      private:
        std::string secret_;
    };

    /// \brief HMAC-SHA512。
    struct HTTPLIB_API hs512 : algorithm
    {
        explicit hs512(std::string_view secret) : secret_(secret) {}
        std::string
        name() const override
        {
            return "HS512";
        }
        std::string sign(std::string_view data, boost::system::error_code& ec) const override;
        std::shared_ptr<algorithm>
        clone() const override
        {
            return std::make_shared<hs512>(secret_);
        }

      private:
        std::string secret_;
    };

} // namespace httplib::jwt
