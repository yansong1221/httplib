#include "httplib/server/middleware/jwt_auth.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <spdlog/spdlog.h>

namespace httplib::server::middleware
{

    jwt_auth::jwt_auth(jwt::algorithm const& alg) : alg_(alg.clone()) {}

    jwt_auth&
    jwt_auth::with_issuer(std::string_view iss)
    {
        issuer_ = iss;
        return *this;
    }

    jwt_auth&
    jwt_auth::with_audience(std::string_view aud)
    {
        audience_ = aud;
        return *this;
    }

    jwt_auth&
    jwt_auth::with_scheme(std::string_view scheme)
    {
        scheme_ = scheme;
        return *this;
    }

    jwt_auth&
    jwt_auth::with_header_name(std::string_view name)
    {
        header_name_ = name;
        return *this;
    }

    bool
    jwt_auth::before(request& req, response& resp)
    {
        std::string_view token;

        if (!header_name_.empty())
        {
            if (token = req[header_name_]; token.empty())
            {
                resp.set_error_content(http::status::unauthorized);
                return false;
            }
        }
        else
        {
            auto auth = req[http::field::authorization];
            auto prefix = scheme_ + " ";
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

        auto verifier = jwt::verify(*alg_);
        if (!issuer_.empty())
        {
            verifier.with_issuer(issuer_);
        }
        if (!audience_.empty())
        {
            verifier.with_audience(audience_);
        }

        boost::system::error_code ec;
        verifier.verify(decoded, ec);
        if (ec)
        {
            resp.set_error_content(http::status::unauthorized);
            return false;
        }

        req.set_custom_data(jwt_key, std::move(decoded));
        return true;
    }

} // namespace httplib::server::middleware
