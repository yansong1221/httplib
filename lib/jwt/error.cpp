#include "httplib/config.hpp"
// 两个错误类别的定义：同一 error 枚举的 boost 侧（公共 make_error_code）
// 与 std 侧（jwt-cpp 的检查器在 std::error_code 世界里回报错误）。

#include "httplib/jwt.hpp"
#include "common.hpp"
#include <string>

namespace httplib::jwt::detail
{
    std::string
    jwt_error_message(int ev) noexcept
    {
        switch (static_cast<error>(ev))
        {
            case error::signature_verification: return "signature verification failed";
            case error::invalid_token: return "token is not a well-formed JWT";
            case error::expired: return "token has expired";
            case error::not_yet_valid: return "token is not yet valid (nbf)";
            case error::algorithm_mismatch: return "token algorithm is not in the allowed list";
            case error::not_initialized: return "verifier has no allowed algorithm configured";
            case error::claim_mismatch: return "claim verification failed";
        }
        return "unknown jwt error";
    }

    struct boost_error_category : boost::system::error_category
    {
        char const*
        name() const noexcept override
        {
            return "httplib.jwt";
        }
        std::string
        message(int ev) const override
        {
            return jwt_error_message(ev);
        }
    };

    boost::system::error_category const&
    jwt_boost_category() noexcept
    {
        static boost_error_category cat;
        return cat;
    }

    // std 侧类别 —— 与 boost 类别对应同一枚举，供 jwt-cpp 的自定义检查器
    //（std::error_code 世界）内写入本库错误码。verify() 一旦遇到非空 ec 就
    // 短路且不限定类别，因此本库错误码可以原样穿透 jwt-cpp 引擎。
    struct std_error_category : std::error_category
    {
        char const*
        name() const noexcept override
        {
            return "httplib.jwt";
        }
        std::string
        message(int ev) const override
        {
            return jwt_error_message(ev);
        }
    };

    std::error_category const&
    jwt_std_category() noexcept
    {
        static std_error_category cat;
        return cat;
    }

    std::error_code
    std_of(error e) noexcept
    {
        return { static_cast<int>(e), jwt_std_category() };
    }
} // namespace httplib::jwt::detail

namespace httplib::jwt
{
    boost::system::error_code
    make_error_code(error e)
    {
        return { static_cast<int>(e), detail::jwt_boost_category() };
    }
} // namespace httplib::jwt
