#pragma once
#include "httplib/server/server_fwd.hpp"
#include "httplib/util/jwt.hpp"
#include <memory>

namespace httplib::server::middleware
{

    class HTTPLIB_API jwt_auth_middleware
    {
      public:
        static constexpr auto key = "jwt";
        using value_type = jwt::decoded_jwt;

        explicit jwt_auth_middleware(jwt::algorithm const& alg);
        jwt_auth_middleware(jwt_auth_middleware const& other);
        jwt_auth_middleware(jwt_auth_middleware&&) noexcept;
        jwt_auth_middleware& operator=(jwt_auth_middleware const& other);
        jwt_auth_middleware& operator=(jwt_auth_middleware&&) noexcept;
        ~jwt_auth_middleware();

        jwt_auth_middleware& with_issuer(std::string_view iss);
        jwt_auth_middleware& with_audience(std::string_view aud);
        jwt_auth_middleware& with_scheme(std::string_view scheme);
        jwt_auth_middleware& with_header_name(std::string_view name);

        bool before(request& req, response& resp);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
