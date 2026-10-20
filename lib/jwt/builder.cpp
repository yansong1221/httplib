#include "httplib/config.hpp"
#include "httplib/jwt.hpp" // 公共 API 恒在：失效桩段也按同一签名定义
#ifdef HTTPLIB_ENABLED_JWT
/// \brief builder —— jwtpp::builder<system_clock, jtraits> 的 PIMPL。
///
/// set_* 全部直通 jwt-cpp；sign() 刷新 alg claim，其余 header claim
///（kid / cty / 自定义）一一保留，换算法重签安全。

#include "impl.hpp"

namespace httplib::jwt
{
    builder::builder() : impl_(std::make_unique<impl>()) {}
    builder::builder(builder const& other) : impl_(std::make_unique<impl>(*other.impl_)) {}
    builder::builder(builder&& other) noexcept = default;
    builder::~builder() = default;
    builder&
    builder::operator=(builder const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(*other.impl_);
        }
        return *this;
    }
    builder& builder::operator=(builder&& other) noexcept = default;

    builder&
    builder::set_type(std::string_view type)
    {
        impl_->engine.set_type(detail::jtraits::string_type { type });
        return *this;
    }
    builder&
    builder::set_content_type(std::string_view cty)
    {
        impl_->engine.set_content_type(detail::jtraits::string_type { cty });
        return *this;
    }
    builder&
    builder::set_key_id(std::string_view kid)
    {
        impl_->engine.set_key_id(detail::jtraits::string_type { kid });
        return *this;
    }
    builder&
    builder::set_header_claim(std::string_view key, boost::json::value value)
    {
        impl_->engine.set_header_claim(detail::jtraits::string_type { key }, value);
        return *this;
    }
    builder&
    builder::set_issuer(std::string_view iss)
    {
        impl_->engine.set_issuer(detail::jtraits::string_type { iss });
        return *this;
    }
    builder&
    builder::set_subject(std::string_view sub)
    {
        impl_->engine.set_subject(detail::jtraits::string_type { sub });
        return *this;
    }
    builder&
    builder::set_audience(std::string_view aud)
    {
        impl_->engine.set_audience(detail::jtraits::string_type { aud });
        return *this;
    }
    builder&
    builder::set_id(std::string_view id)
    {
        impl_->engine.set_id(detail::jtraits::string_type { id });
        return *this;
    }
    builder&
    builder::set_issued_at(std::chrono::system_clock::time_point t)
    {
        impl_->engine.set_issued_at(t);
        return *this;
    }
    builder&
    builder::set_issued_now()
    {
        impl_->engine.set_issued_now();
        return *this;
    }
    builder&
    builder::set_not_before(std::chrono::system_clock::time_point t)
    {
        impl_->engine.set_not_before(t);
        return *this;
    }
    builder&
    builder::set_expires_at(std::chrono::system_clock::time_point t)
    {
        impl_->engine.set_expires_at(t);
        return *this;
    }
    builder&
    builder::set_payload_claim(std::string_view key, boost::json::value value)
    {
        impl_->engine.set_payload_claim(detail::jtraits::string_type { key }, value);
        return *this;
    }

    boost::system::result<std::string>
    builder::sign(algorithm const& alg)
    {
        // alg 恒随 sign 刷新（换算法重签安全）；其余 header claim ——
        // kid / cty / 自定义 —— jwt-cpp builder 不清空，全部保留。
        impl_->engine.set_algorithm(alg.name());

        std::error_code sec;
        auto token = impl_->engine.sign(detail::algo_adapter { alg.clone() }, sec);
        if (sec)
        {
            return make_error_code(error::signature_verification);
        }
        return token;
    }

} // namespace httplib::jwt

#else // !HTTPLIB_ENABLED_JWT —— 运行期失效桩（同 use_ssl 形态）

#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
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

    class builder::impl
    {
    };

    builder::builder() { unavailable(); }
    builder::builder(builder const&) : impl_(nullptr) { unavailable(); }
    builder&
    builder::operator=(builder const&)
    {
        unavailable();
    }
    builder::builder(builder&& other) noexcept : impl_(std::move(other.impl_))
    {
        unavailable_noexcept();
    }
    builder&
    builder::operator=(builder&&) noexcept
    {
        unavailable_noexcept();
    }
    builder::~builder() = default;

    builder&
    builder::set_type(std::string_view)
    {
        unavailable();
    }
    builder&
    builder::set_content_type(std::string_view)
    {
        unavailable();
    }
    builder&
    builder::set_key_id(std::string_view)
    {
        unavailable();
    }
    builder&
    builder::set_header_claim(std::string_view, boost::json::value)
    {
        unavailable();
    }
    builder&
    builder::set_issuer(std::string_view)
    {
        unavailable();
    }
    builder&
    builder::set_subject(std::string_view)
    {
        unavailable();
    }
    builder&
    builder::set_audience(std::string_view)
    {
        unavailable();
    }
    builder&
    builder::set_id(std::string_view)
    {
        unavailable();
    }
    builder&
    builder::set_issued_at(std::chrono::system_clock::time_point)
    {
        unavailable();
    }
    builder&
    builder::set_issued_now()
    {
        unavailable();
    }
    builder&
    builder::set_not_before(std::chrono::system_clock::time_point)
    {
        unavailable();
    }
    builder&
    builder::set_expires_at(std::chrono::system_clock::time_point)
    {
        unavailable();
    }
    builder&
    builder::set_payload_claim(std::string_view, boost::json::value)
    {
        unavailable();
    }
    boost::system::result<std::string>
    builder::sign(algorithm const& alg)
    {
        (void)alg;
        return boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported);
    }

} // namespace httplib::jwt
#endif // HTTPLIB_ENABLED_JWT
