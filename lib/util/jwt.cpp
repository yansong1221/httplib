#include "httplib/util/jwt.hpp"
#include <boost/beast/core/detail/base64.hpp>
#include <span>
#include <stdexcept>
#include <string>

#ifdef HTTPLIB_ENABLED_SSL
#include <openssl/hmac.h>
#endif

namespace httplib::jwt
{
    namespace
    {
        struct error_category : boost::system::error_category
        {
            char const*
            name() const noexcept override
            {
                return "jwt";
            }
            std::string
            message(int ev) const override
            {
                switch (static_cast<error>(ev))
                {
                    case error::signature_verification:
                        return "signature verification failed";
                    case error::issuer_mismatch:
                        return "issuer mismatch";
                    case error::subject_mismatch:
                        return "subject mismatch";
                    case error::audience_mismatch:
                        return "audience mismatch";
                    case error::id_mismatch:
                        return "id mismatch";
                    case error::invalid_token:
                        return "invalid token";
                    case error::expired:
                        return "token expired";
                    case error::not_yet_valid:
                        return "token not yet valid";
                    case error::algorithm_mismatch:
                        return "algorithm mismatch";
                }
                return "unknown error";
            }
        };

        boost::system::error_category const&
        jwt_category() noexcept
        {
            static error_category cat;
            return cat;
        }
    } // namespace

    boost::system::error_code
    make_error_code(error e)
    {
        return { static_cast<int>(e), jwt_category() };
    }
    namespace
    {

        std::string
        base64url_encode(std::span<uint8_t const> data)
        {
            std::string result;
            result.resize(beast::detail::base64::encoded_size(data.size()));
            beast::detail::base64::encode(result.data(), data.data(), data.size());
            while (!result.empty() && result.back() == '=')
            {
                result.pop_back();
            }
            for (auto& c : result)
            {
                if (c == '+')
                {
                    c = '-';
                }
                else if (c == '/')
                {
                    c = '_';
                }
            }
            return result;
        }

        inline std::string
        base64url_encode(std::string_view data)
        {
            return base64url_encode(std::span((uint8_t const*)data.data(), data.size()));
        }

        std::string
        base64url_decode(std::string_view data)
        {
            std::string padded(data);
            while (padded.size() % 4 != 0)
            {
                padded += '=';
            }
            for (auto& c : padded)
            {
                if (c == '-')
                {
                    c = '+';
                }
                else if (c == '_')
                {
                    c = '/';
                }
            }
            std::string result;
            result.resize(beast::detail::base64::decoded_size(padded.size()));
            auto [n, _] = beast::detail::base64::decode(result.data(), padded.data(), padded.size());
            result.resize(n);
            return result;
        }

    } // namespace

    std::string
    hs256::sign(std::string_view data) const
    {
#ifdef HTTPLIB_ENABLED_SSL
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), secret_.data(), (int)secret_.size(), (unsigned char*)data.data(), data.size(), hash, &len);
        return base64url_encode(std::span(hash, len));
#else
        (void)data;
        throw std::runtime_error("SSL required for HS256");
#endif
    }

    std::string
    hs384::sign(std::string_view data) const
    {
#ifdef HTTPLIB_ENABLED_SSL
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha384(), secret_.data(), (int)secret_.size(), (unsigned char*)data.data(), data.size(), hash, &len);
        return base64url_encode(std::span(hash, len));
#else
        (void)data;
        throw std::runtime_error("SSL required for HS384");
#endif
    }

    std::string
    hs512::sign(std::string_view data) const
    {
#ifdef HTTPLIB_ENABLED_SSL
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha512(), secret_.data(), (int)secret_.size(), (unsigned char*)data.data(), data.size(), hash, &len);
        return base64url_encode(std::span(hash, len));
#else
        (void)data;
        throw std::runtime_error("SSL required for HS512");
#endif
    }

    decoded_jwt::decoded_jwt(std::string_view token) : token_(token)
    {
        auto dot1 = token.find('.');
        if (dot1 == std::string_view::npos)
        {
            return;
        }
        auto dot2 = token.find('.', dot1 + 1);
        if (dot2 == std::string_view::npos)
        {
            return;
        }

        signature_ = token.substr(dot2 + 1);
        header_ = boost::json::parse(base64url_decode(token.substr(0, dot1)));
        payload_ = boost::json::parse(base64url_decode(token.substr(dot1 + 1, dot2 - dot1 - 1)));
        algorithm_ = boost::json::value_to<std::string>(header_.at(claim::algorithm));
    }

    bool
    decoded_jwt::has_issuer() const
    {
        return payload_.is_object() && payload_.as_object().contains(claim::issuer);
    }
    bool
    decoded_jwt::has_subject() const
    {
        return payload_.is_object() && payload_.as_object().contains(claim::subject);
    }
    bool
    decoded_jwt::has_audience() const
    {
        return payload_.is_object() && payload_.as_object().contains(claim::audience);
    }
    bool
    decoded_jwt::has_id() const
    {
        return payload_.is_object() && payload_.as_object().contains(claim::id);
    }
    bool
    decoded_jwt::has_issued_at() const
    {
        return payload_.is_object() && payload_.as_object().contains(claim::issued_at);
    }
    bool
    decoded_jwt::has_not_before() const
    {
        return payload_.is_object() && payload_.as_object().contains(claim::not_before);
    }
    bool
    decoded_jwt::has_expires_at() const
    {
        return payload_.is_object() && payload_.as_object().contains(claim::expires_at);
    }
    bool
    decoded_jwt::has_type() const
    {
        return header_.is_object() && header_.as_object().contains(claim::type);
    }

    std::string
    decoded_jwt::get_issuer() const
    {
        return get_payload_claim<std::string>(claim::issuer);
    }
    std::string
    decoded_jwt::get_subject() const
    {
        return get_payload_claim<std::string>(claim::subject);
    }
    std::string
    decoded_jwt::get_audience() const
    {
        return get_payload_claim<std::string>(claim::audience);
    }
    std::string
    decoded_jwt::get_id() const
    {
        return get_payload_claim<std::string>(claim::id);
    }
    std::string
    decoded_jwt::get_type() const
    {
        return boost::json::value_to<std::string>(header_.at(claim::type));
    }
    std::chrono::system_clock::time_point
    decoded_jwt::get_issued_at() const
    {
        return std::chrono::system_clock::from_time_t((time_t)get_payload_claim<int64_t>(claim::issued_at));
    }
    std::chrono::system_clock::time_point
    decoded_jwt::get_not_before() const
    {
        return std::chrono::system_clock::from_time_t((time_t)get_payload_claim<int64_t>(claim::not_before));
    }
    std::chrono::system_clock::time_point
    decoded_jwt::get_expires_at() const
    {
        return std::chrono::system_clock::from_time_t((time_t)get_payload_claim<int64_t>(claim::expires_at));
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
        auto dot1 = token_.find('.');
        auto dot2 = token_.find('.', dot1 + 1);
        auto msg = std::string_view(token_).substr(0, dot2);
        auto ok = (signature_ == alg.sign(msg));
        if (!ok)
        {
            ec = make_error_code(error::signature_verification);
        }
        else
        {
            ec.clear();
        }
        return ok;
    }

    builder&
    builder::set_type(std::string_view type)
    {
        type_ = type;
        return *this;
    }

    builder&
    builder::set_content_type(std::string_view cty)
    {
        return set_header_claim(claim::content_type, boost::json::value(std::string(cty)));
    }

    builder&
    builder::set_key_id(std::string_view kid)
    {
        return set_header_claim(claim::key_id, boost::json::value(std::string(kid)));
    }

    builder&
    builder::set_header_claim(std::string_view key, boost::json::value value)
    {
        header_[std::string(key)] = std::move(value);
        return *this;
    }

    builder&
    builder::set_issuer(std::string_view iss)
    {
        return set_payload_claim(claim::issuer, boost::json::value(std::string(iss)));
    }

    builder&
    builder::set_subject(std::string_view sub)
    {
        return set_payload_claim(claim::subject, boost::json::value(std::string(sub)));
    }

    builder&
    builder::set_audience(std::string_view aud)
    {
        return set_payload_claim(claim::audience, boost::json::value(std::string(aud)));
    }

    builder&
    builder::set_id(std::string_view id)
    {
        return set_payload_claim(claim::id, boost::json::value(std::string(id)));
    }

    builder&
    builder::set_issued_now()
    {
        return set_issued_at(clock_.now());
    }

    builder&
    builder::set_issued_at(std::chrono::system_clock::time_point t)
    {
        iat_ = t;
        return *this;
    }

    builder&
    builder::set_not_before(std::chrono::system_clock::time_point t)
    {
        nbf_ = t;
        return *this;
    }

    builder&
    builder::set_expires_at(std::chrono::system_clock::time_point t)
    {
        exp_ = t;
        return *this;
    }

    builder&
    builder::set_payload_claim(std::string_view key, boost::json::value value)
    {
        claims_[std::string(key)] = std::move(value);
        return *this;
    }

    std::string
    builder::sign(algorithm const& alg)
    {
        auto to_epoch
            = [](auto tp) { return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count(); };

        header_ = {
            { claim::algorithm, alg.name() }
        };
        if (!type_.empty())
        {
            header_.emplace(claim::type, type_);
        }

        if (iat_ != std::chrono::system_clock::time_point {})
        {
            payload_.emplace(claim::issued_at, to_epoch(iat_));
        }
        if (nbf_ != std::chrono::system_clock::time_point {})
        {
            payload_.emplace(claim::not_before, to_epoch(nbf_));
        }
        if (exp_ != std::chrono::system_clock::time_point {})
        {
            payload_.emplace(claim::expires_at, to_epoch(exp_));
        }
        for (auto& [k, v] : claims_)
        {
            payload_.emplace(k, std::move(v));
        }

        auto header_b64 = base64url_encode(boost::json::serialize(header_));
        auto payload_b64 = base64url_encode(boost::json::serialize(payload_));
        auto signature_b64 = alg.sign(std::string(header_b64) + "." + std::string(payload_b64));

        return std::string(header_b64) + "." + std::string(payload_b64) + "." + signature_b64;
    }

    boost::system::result<decoded_jwt>
    decode(std::string_view token)
    {
        auto dot1 = token.find('.');
        auto dot2 = dot1 != std::string_view::npos ? token.find('.', dot1 + 1) : std::string_view::npos;
        if (dot1 == std::string_view::npos || dot2 == std::string_view::npos)
        {
            return make_error_code(error::invalid_token);
        }
        try
        {
            return decoded_jwt(token);
        }
        catch (std::exception const&)
        {
            return make_error_code(error::invalid_token);
        }
    }

    verifier&
    verifier::allow_algorithm(algorithm const& alg)
    {
        alg_ = alg.clone();
        return *this;
    }

    verifier&
    verifier::with_issuer(std::string_view iss)
    {
        issuer_ = iss;
        return *this;
    }

    verifier&
    verifier::with_subject(std::string_view sub)
    {
        subject_ = sub;
        return *this;
    }

    verifier&
    verifier::with_audience(std::string_view aud)
    {
        audience_ = aud;
        return *this;
    }

    verifier&
    verifier::with_id(std::string_view id)
    {
        id_ = id;
        return *this;
    }

    verifier&
    verifier::with_claim(std::string_view key, std::function<bool(boost::json::value const&)> fn)
    {
        custom_checks_.emplace_back(std::string(key), std::move(fn));
        return *this;
    }

    verifier&
    verifier::with_clock_skew(std::chrono::seconds skew)
    {
        clock_skew_ = skew;
        return *this;
    }

    void
    verifier::verify(decoded_jwt const& jwt, boost::system::error_code& ec) const
    {
        ec.clear();

        if (alg_)
        {
            auto jwt_alg = jwt.get_algorithm();
            if (jwt_alg != alg_->name())
            {
                ec = make_error_code(error::algorithm_mismatch);
                return;
            }
            if (!jwt.verify(*alg_, ec))
            {
                return;
            }
        }

        auto now = std::chrono::system_clock::now();

        if (jwt.has_expires_at())
        {
            auto exp = jwt.get_expires_at();
            if (now > exp + clock_skew_)
            {
                ec = make_error_code(error::expired);
                return;
            }
        }

        if (jwt.has_not_before())
        {
            auto nbf = jwt.get_not_before();
            if (now < nbf - clock_skew_)
            {
                ec = make_error_code(error::not_yet_valid);
                return;
            }
        }

        if (!issuer_.empty() && jwt.get_issuer() != issuer_)
        {
            ec = make_error_code(error::issuer_mismatch);
            return;
        }
        if (!subject_.empty() && jwt.get_subject() != subject_)
        {
            ec = make_error_code(error::subject_mismatch);
            return;
        }
        if (!audience_.empty() && jwt.get_audience() != audience_)
        {
            ec = make_error_code(error::audience_mismatch);
            return;
        }
        if (!id_.empty() && jwt.get_id() != id_)
        {
            ec = make_error_code(error::id_mismatch);
            return;
        }
        for (auto& [key, fn] : custom_checks_)
        {
            auto payload = jwt.get_payload();
            auto it = payload.as_object().find(key);
            if (it == payload.as_object().end() || !fn(it->value()))
            {
                ec = make_error_code(error::signature_verification);
                return;
            }
        }
    }

} // namespace httplib::jwt
