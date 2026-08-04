#pragma once
#include "httplib/server/server_fwd.hpp"
#include "httplib/util/jwt.hpp"
#include <string>

namespace httplib::server::middleware
{

    class HTTPLIB_API jwt_auth
    {
      public:
        static constexpr auto jwt_key = "jwt";

        explicit jwt_auth(jwt::algorithm const& alg);
        jwt_auth& with_issuer(std::string_view iss);
        jwt_auth& with_audience(std::string_view aud);
        jwt_auth& with_scheme(std::string_view scheme);
        jwt_auth& with_header_name(std::string_view name);

        bool before(request& req, response& resp);

      private:
        std::shared_ptr<jwt::algorithm const> alg_;
        std::string issuer_;
        std::string audience_;
        std::string scheme_ = "Bearer";
        std::string header_name_;
    };

} // namespace httplib::server::middleware
