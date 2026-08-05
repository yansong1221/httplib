#include "httplib/server/middleware/jwt_auth.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <spdlog/spdlog.h>
#include <string>

namespace httplib::server::middleware
{

    struct jwt_auth_middleware::impl
    {
        std::shared_ptr<jwt::algorithm const> alg;
        std::string issuer;
        std::string audience;
        std::string scheme = "Bearer";
        std::string header_name;
    };

    jwt_auth_middleware::jwt_auth_middleware(jwt::algorithm const& alg) : impl_(std::make_unique<impl>())
    {
        impl_->alg = alg.clone();
    }
    jwt_auth_middleware::jwt_auth_middleware(jwt_auth_middleware const& other)
        : impl_(std::make_unique<impl>(*other.impl_))
    {
    }
    jwt_auth_middleware::jwt_auth_middleware(jwt_auth_middleware&&) noexcept = default;
    jwt_auth_middleware::~jwt_auth_middleware() = default;
    jwt_auth_middleware&
    jwt_auth_middleware::operator=(jwt_auth_middleware const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(*other.impl_);
        }
        return *this;
    }
    jwt_auth_middleware& jwt_auth_middleware::operator=(jwt_auth_middleware&&) noexcept = default;

    jwt_auth_middleware&
    jwt_auth_middleware::with_issuer(std::string_view iss)
    {
        impl_->issuer = iss;
        return *this;
    }

    jwt_auth_middleware&
    jwt_auth_middleware::with_audience(std::string_view aud)
    {
        impl_->audience = aud;
        return *this;
    }

    jwt_auth_middleware&
    jwt_auth_middleware::with_scheme(std::string_view scheme)
    {
        impl_->scheme = scheme;
        return *this;
    }

    jwt_auth_middleware&
    jwt_auth_middleware::with_header_name(std::string_view name)
    {
        impl_->header_name = name;
        return *this;
    }

    bool
    jwt_auth_middleware::before(request& req, response& resp)
    {
        auto& p = *impl_;
        std::string_view token;

        if (!p.header_name.empty())
        {
            if (token = req[p.header_name]; token.empty())
            {
                resp.set_error_content(http::status::unauthorized);
                return false;
            }
        }
        else
        {
            auto auth = req[http::field::authorization];
            auto prefix = p.scheme + " ";
            if (!auth.starts_with(prefix))
            {
                resp.set_error_content(http::status::unauthorized);
                return false;
            }
            token = auth.substr(prefix.size());
        }
        auto result = jwt::decode(token);
        if (result.has_error())
        {
            resp.set_error_content(http::status::unauthorized);
            return false;
        }
        auto& decoded = result.value();

        auto verifier = jwt::verify(*p.alg);
        if (!p.issuer.empty())
        {
            verifier.with_issuer(p.issuer);
        }
        if (!p.audience.empty())
        {
            verifier.with_audience(p.audience);
        }

        boost::system::error_code ec;
        verifier.verify(decoded, ec);
        if (ec)
        {
            resp.set_error_content(http::status::unauthorized);
            return false;
        }

        req.set_custom_data(key, std::move(decoded));
        return true;
    }

} // namespace httplib::server::middleware
