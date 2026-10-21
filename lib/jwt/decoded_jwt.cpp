#include "httplib/config.hpp"
#include "httplib/jwt.hpp" // 公共 API 恒在：失效桩段也按同一签名定义
#ifdef HTTPLIB_ENABLED_JWT
/// \brief decoded_jwt —— jwtpp::decoded_jwt<jtraits> 的 PIMPL。
///
/// 解析契约：除 std::bad_alloc 外不抛。jwt-cpp 的构造对坏 token 抛
/// std::invalid_argument / std::runtime_error / std::bad_cast，在 impl
/// 构造期统一转成 error::invalid_token（含缺 alg / alg 非字符串）。

#include "impl.hpp"
#include <algorithm>
#include <string_view>
#include <utility>

namespace httplib::jwt
{
    using detail::jdecoded;

    decoded_jwt::decoded_jwt(std::string_view token) : impl_(std::make_unique<impl>(token)) {}
    decoded_jwt::decoded_jwt(decoded_jwt const& other) : impl_(std::make_unique<impl>(*other.impl_)) {}
    decoded_jwt::decoded_jwt(decoded_jwt&& other) noexcept = default;
    decoded_jwt::~decoded_jwt() = default;
    decoded_jwt&
    decoded_jwt::operator=(decoded_jwt const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(*other.impl_);
        }
        return *this;
    }
    decoded_jwt& decoded_jwt::operator=(decoded_jwt&& other) noexcept = default;

    boost::system::error_code
    decoded_jwt::get_error() const
    {
        return impl_->error_;
    }

    boost::json::value
    decoded_jwt::get_payload() const
    {
        if (!impl_->inner) return {};
        return boost::json::value { impl_->inner->get_payload_json() };
    }

    boost::json::value
    decoded_jwt::get_header() const
    {
        if (!impl_->inner) return {};
        return boost::json::value { impl_->inner->get_header_json() };
    }

    std::string_view
    decoded_jwt::get_token() const
    {
        if (!impl_->inner) return {};
        return impl_->inner->get_token();
    }

    std::string_view
    decoded_jwt::get_signature() const
    {
        if (!impl_->inner) return {};
        return impl_->inner->get_signature_base64();
    }

    std::string_view
    decoded_jwt::get_algorithm() const
    {
        return impl_->alg;
    }

    bool
    decoded_jwt::has_issuer() const
    {
        return impl_->inner && impl_->inner->has_issuer();
    }
    bool
    decoded_jwt::has_subject() const
    {
        return impl_->inner && impl_->inner->has_subject();
    }
    bool
    decoded_jwt::has_audience() const
    {
        return impl_->inner && impl_->inner->has_audience();
    }
    bool
    decoded_jwt::has_id() const
    {
        return impl_->inner && impl_->inner->has_id();
    }
    bool
    decoded_jwt::has_issued_at() const
    {
        return impl_->inner && impl_->inner->has_issued_at();
    }
    bool
    decoded_jwt::has_not_before() const
    {
        return impl_->inner && impl_->inner->has_not_before();
    }
    bool
    decoded_jwt::has_expires_at() const
    {
        return impl_->inner && impl_->inner->has_expires_at();
    }
    bool
    decoded_jwt::has_type() const
    {
        return impl_->inner && impl_->inner->has_type();
    }

    std::string
    decoded_jwt::get_issuer() const
    {
        return impl_->inner->get_issuer();
    }
    std::string
    decoded_jwt::get_subject() const
    {
        return impl_->inner->get_subject();
    }
    std::string
    decoded_jwt::get_audience() const
    {
        // jwt-cpp 把字符串与数组两种形态折叠成 set —— 单值返回原值，多值按逗号
        // 连接（公共 API 的 string 形态；多 audience 精确校验走 verifier）。
        auto aud = impl_->inner->get_audience();
        if (aud.size() <= 1)
        {
            return aud.empty() ? std::string() : *aud.begin();
        }
        std::string out;
        for (auto const& a : aud)
        {
            out += (out.empty() ? "" : ",");
            out += a;
        }
        return out;
    }
    std::string
    decoded_jwt::get_id() const
    {
        return impl_->inner->get_id();
    }
    std::string
    decoded_jwt::get_type() const
    {
        try
        {
            return impl_->inner->get_type();
        }
        catch (std::exception const&)
        {
            return {};
        }
    }
    std::chrono::system_clock::time_point
    decoded_jwt::get_issued_at() const
    {
        return impl_->inner->get_issued_at();
    }
    std::chrono::system_clock::time_point
    decoded_jwt::get_not_before() const
    {
        return impl_->inner->get_not_before();
    }
    std::chrono::system_clock::time_point
    decoded_jwt::get_expires_at() const
    {
        return impl_->inner->get_expires_at();
    }

    bool
    decoded_jwt::verify(algorithm const& alg) const
    {
        boost::system::error_code ec;
        return verify(alg, ec);
    }

    bool
    decoded_jwt::verify(algorithm const& alg, boost::system::error_code& ec) const
    {
        ec.clear();
        auto const& inner = impl_->inner;
        if (!inner)
        {
            ec = impl_->error_;
            return false;
        }

        // 与 jwt-cpp 引擎一致的签名协议：待签数据 = b64(header) + "." + b64(payload)。
        std::string const data = inner->get_header_base64() + "." + inner->get_payload_base64();

        boost::system::error_code sec;
        std::string expected;
        try
        {
            expected = alg.sign(data, sec);
        }
        catch (std::bad_alloc const&)
        {
            throw;
        }
        catch (std::exception const&)
        {
            ec = make_error_code(error::signature_verification);
            return false;
        }
        if (sec)
        {
            // alg 报告的已是公共 boost 契约的错误码，原样穿透
            ec = sec;
            return false;
        }

        std::string actual;
        try
        {
            actual = detail::decode_base64url(std::string(inner->get_signature_base64()));
        }
        catch (std::bad_alloc const&)
        {
            throw;
        }
        catch (std::exception const&)
        {
            // 签名段不是合法 base64url（构造期已最小检查，防御性兜底）
            ec = make_error_code(error::signature_verification);
            return false;
        }

        // 常量时间全字节比较（不因首个差异位置提前返回）。
        bool matched = expected.size() == actual.size();
        for (size_t i = 0; i < (std::min)(expected.size(), actual.size()); i++)
        {
            if (expected[i] != actual[i]) matched = false;
        }
        if (!matched)
        {
            ec = make_error_code(error::signature_verification);
            return false;
        }
        return true;
    }

    boost::system::result<decoded_jwt>
    decode(std::string_view token)
    {
        decoded_jwt d(token);
        if (auto ec = d.get_error())
        {
            return ec;
        }
        return d;
    }

} // namespace httplib::jwt

#else // !HTTPLIB_ENABLED_JWT —— 运行期失效桩（同 use_ssl 形态）

#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "unavailable.hpp"

namespace httplib::jwt
{
    using detail::unavailable;
    using detail::unavailable_noexcept;

    // PIMPL 替身：未启用构建里补齐 unique_ptr<impl> 的完整性（真实 impl 定义在
    // impl.hpp，仅启用构建可见）。impl 只以指针形态出现，空类布局兼容；
    // 与真实实现跨构建互斥编译，无 ODR 冲突。
    class decoded_jwt::impl
    {
    };

    decoded_jwt::decoded_jwt(std::string_view) { unavailable(); }
    decoded_jwt::decoded_jwt(decoded_jwt const&) : impl_(nullptr) { unavailable(); }
    decoded_jwt&
    decoded_jwt::operator=(decoded_jwt const&)
    {
        unavailable();
    }
    decoded_jwt::decoded_jwt(decoded_jwt&& other) noexcept : impl_(std::move(other.impl_))
    {
        unavailable_noexcept();
    }
    decoded_jwt&
    decoded_jwt::operator=(decoded_jwt&&) noexcept
    {
        unavailable_noexcept();
    }
    decoded_jwt::~decoded_jwt() = default;

    boost::system::error_code
    decoded_jwt::get_error() const
    {
        unavailable();
    }
    boost::json::value
    decoded_jwt::get_payload() const
    {
        unavailable();
    }
    boost::json::value
    decoded_jwt::get_header() const
    {
        unavailable();
    }
    std::string_view
    decoded_jwt::get_token() const
    {
        unavailable();
    }
    std::string_view
    decoded_jwt::get_signature() const
    {
        unavailable();
    }
    std::string_view
    decoded_jwt::get_algorithm() const
    {
        unavailable();
    }
    bool
    decoded_jwt::has_issuer() const
    {
        unavailable();
    }
    bool
    decoded_jwt::has_subject() const
    {
        unavailable();
    }
    bool
    decoded_jwt::has_audience() const
    {
        unavailable();
    }
    bool
    decoded_jwt::has_id() const
    {
        unavailable();
    }
    bool
    decoded_jwt::has_issued_at() const
    {
        unavailable();
    }
    bool
    decoded_jwt::has_not_before() const
    {
        unavailable();
    }
    bool
    decoded_jwt::has_expires_at() const
    {
        unavailable();
    }
    bool
    decoded_jwt::has_type() const
    {
        unavailable();
    }
    std::string
    decoded_jwt::get_issuer() const
    {
        unavailable();
    }
    std::string
    decoded_jwt::get_subject() const
    {
        unavailable();
    }
    std::string
    decoded_jwt::get_audience() const
    {
        unavailable();
    }
    std::string
    decoded_jwt::get_id() const
    {
        unavailable();
    }
    std::string
    decoded_jwt::get_type() const
    {
        unavailable();
    }
    std::chrono::system_clock::time_point
    decoded_jwt::get_issued_at() const
    {
        unavailable();
    }
    std::chrono::system_clock::time_point
    decoded_jwt::get_not_before() const
    {
        unavailable();
    }
    std::chrono::system_clock::time_point
    decoded_jwt::get_expires_at() const
    {
        unavailable();
    }
    bool
    decoded_jwt::verify(algorithm const&) const
    {
        unavailable();
    }
    bool
    decoded_jwt::verify(algorithm const&, boost::system::error_code& ec) const
    {
        ec = boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported);
        return false;
    }

    boost::system::result<decoded_jwt>
    decode(std::string_view token)
    {
        (void)token;
        return boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported);
    }

} // namespace httplib::jwt
#endif // HTTPLIB_ENABLED_JWT
