#pragma once
#include "httplib/config.hpp"
#include <boost/json.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace httplib::jwt
{

    namespace claim
    {
        constexpr auto issuer = "iss";
        constexpr auto subject = "sub";
        constexpr auto audience = "aud";
        constexpr auto id = "jti";
        constexpr auto issued_at = "iat";
        constexpr auto not_before = "nbf";
        constexpr auto expires_at = "exp";
        constexpr auto algorithm = "alg";
        constexpr auto type = "typ";
        constexpr auto content_type = "cty";
        constexpr auto key_id = "kid";
    } // namespace claim

    enum class error
    {
        signature_verification = 1,
        issuer_mismatch,
        subject_mismatch,
        audience_mismatch,
        id_mismatch,
        invalid_token,
    };

    HTTPLIB_API boost::system::error_code make_error_code(error e);

    class HTTPLIB_API algorithm
    {
      public:
        virtual ~algorithm() = default;
        virtual std::string name() const = 0;
        virtual std::string sign(std::string_view data) const = 0;
        virtual std::shared_ptr<algorithm> clone() const = 0;
    };

    struct HTTPLIB_API hs256 : algorithm
    {
        explicit hs256(std::string_view secret) : secret_(secret) {}
        std::string
        name() const override
        {
            return "HS256";
        }
        std::string sign(std::string_view data) const override;
        std::shared_ptr<algorithm>
        clone() const override
        {
            return std::make_shared<hs256>(secret_);
        }

      private:
        std::string secret_;
    };

    struct HTTPLIB_API hs384 : algorithm
    {
        explicit hs384(std::string_view secret) : secret_(secret) {}
        std::string
        name() const override
        {
            return "HS384";
        }
        std::string sign(std::string_view data) const override;
        std::shared_ptr<algorithm>
        clone() const override
        {
            return std::make_shared<hs384>(secret_);
        }

      private:
        std::string secret_;
    };

    struct HTTPLIB_API hs512 : algorithm
    {
        explicit hs512(std::string_view secret) : secret_(secret) {}
        std::string
        name() const override
        {
            return "HS512";
        }
        std::string sign(std::string_view data) const override;
        std::shared_ptr<algorithm>
        clone() const override
        {
            return std::make_shared<hs512>(secret_);
        }

      private:
        std::string secret_;
    };

    class builder;

    class HTTPLIB_API decoded_jwt
    {
      public:
        decoded_jwt(std::string_view token);

        boost::json::value
        get_payload() const
        {
            return payload_;
        }
        boost::json::value
        get_header() const
        {
            return header_;
        }
        std::string_view
        get_token() const
        {
            return token_;
        }
        std::string_view
        get_signature() const
        {
            return signature_;
        }
        std::string_view
        get_algorithm() const
        {
            return algorithm_;
        }

        bool has_issuer() const;
        bool has_subject() const;
        bool has_audience() const;
        bool has_id() const;
        bool has_issued_at() const;
        bool has_not_before() const;
        bool has_expires_at() const;
        bool has_type() const;
        std::string get_issuer() const;
        std::string get_subject() const;
        std::string get_audience() const;
        std::string get_id() const;
        std::string get_type() const;
        std::chrono::system_clock::time_point get_issued_at() const;
        std::chrono::system_clock::time_point get_not_before() const;
        std::chrono::system_clock::time_point get_expires_at() const;

        template <typename T>
        auto
        get_payload_claim(std::string_view key) const
        {
            return boost::json::value_to<T>(payload_.at(key));
        }

        bool verify(algorithm const& alg) const;
        bool verify(algorithm const& alg, boost::system::error_code& ec) const;

      private:
        friend class builder;
        std::string token_;
        std::string signature_;
        boost::json::value header_;
        boost::json::value payload_;
        std::string algorithm_;
    };

    class HTTPLIB_API builder
    {
      public:
        builder& set_type(std::string_view type);
        builder& set_content_type(std::string_view cty);
        builder& set_key_id(std::string_view kid);
        builder& set_header_claim(std::string_view key, boost::json::value value);
        builder& set_issuer(std::string_view iss);
        builder& set_subject(std::string_view sub);
        builder& set_audience(std::string_view aud);
        builder& set_id(std::string_view id);
        builder& set_issued_at(std::chrono::system_clock::time_point t);
        builder& set_issued_now();
        builder& set_not_before(std::chrono::system_clock::time_point t);
        builder& set_expires_at(std::chrono::system_clock::time_point t);
        template <typename Rep, typename Period>
        builder&
        set_expires_in(std::chrono::duration<Rep, Period> d)
        {
            return set_expires_at(clock_.now() + d);
        }
        builder& set_payload_claim(std::string_view key, boost::json::value value);
        std::string sign(algorithm const& alg);

      private:
        boost::json::object header_;
        boost::json::object payload_;
        std::unordered_map<std::string, boost::json::value> claims_;
        std::string type_;
        std::chrono::system_clock clock_ {};
        std::chrono::system_clock::time_point iat_ {};
        std::chrono::system_clock::time_point nbf_ {};
        std::chrono::system_clock::time_point exp_ {};
    };

    inline builder
    create()
    {
        return builder {};
    }

    HTTPLIB_API boost::system::result<decoded_jwt> decode(std::string_view token);
    template <std::convertible_to<std::string_view> S>
    boost::system::result<decoded_jwt>
    decode(S&& token)
    {
        return decode(std::string_view(std::forward<S>(token)));
    }

    class HTTPLIB_API verifier
    {
      public:
        verifier& allow_algorithm(algorithm const& alg);
        verifier& with_issuer(std::string_view iss);
        verifier& with_subject(std::string_view sub);
        verifier& with_audience(std::string_view aud);
        verifier& with_id(std::string_view id);
        verifier& with_claim(std::string_view key, std::function<bool(boost::json::value const&)> fn);
        void
        verify(decoded_jwt const& jwt) const
        {
            boost::system::error_code ec;
            verify(jwt, ec);
            if (ec)
            {
                throw std::system_error(ec);
            }
        }
        void verify(decoded_jwt const& jwt, boost::system::error_code& ec) const;

      private:
        std::shared_ptr<algorithm> alg_;
        std::string issuer_;
        std::string subject_;
        std::string audience_;
        std::string id_;
        std::vector<std::pair<std::string, std::function<bool(boost::json::value const&)>>> custom_checks_;
    };

    inline verifier
    verify(algorithm const& alg)
    {
        verifier v;
        v.allow_algorithm(alg);
        return v;
    }

} // namespace httplib::jwt
