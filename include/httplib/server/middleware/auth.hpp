#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::server::middleware
{

    class HTTPLIB_API basic_auth
    {
      public:
        using validator_t = std::function<bool(std::string_view username, std::string_view password)>;

        explicit basic_auth(validator_t validator, std::string realm = "Restricted");
        ~basic_auth();
        basic_auth(basic_auth const&);
        basic_auth& operator=(basic_auth const&);
        basic_auth(basic_auth&&) noexcept;
        basic_auth& operator=(basic_auth&&) noexcept;

        bool before(request& req, response& resp);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

    class HTTPLIB_API bearer_auth
    {
      public:
        using validator_t = std::function<bool(std::string_view token)>;

        explicit bearer_auth(validator_t validator, std::string realm = "Restricted");
        ~bearer_auth();
        bearer_auth(bearer_auth const&);
        bearer_auth& operator=(bearer_auth const&);
        bearer_auth(bearer_auth&&) noexcept;
        bearer_auth& operator=(bearer_auth&&) noexcept;

        bool before(request& req, response& resp);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
