#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::server::middleware
{

    class HTTPLIB_API cors
    {
      public:
        cors();
        ~cors();
        cors(cors const& other);
        cors& operator=(cors const& other);
        cors(cors&&) noexcept;
        cors& operator=(cors&&) noexcept;

        cors& allow_origin(std::string origin);
        cors& allow_origins(std::vector<std::string> origins);
        cors& allow_methods(std::vector<std::string> methods);
        cors& allow_headers(std::vector<std::string> headers);
        cors& allow_credentials(bool allow);
        cors& max_age(int seconds);

        bool before(request& req, response& resp);
        bool after(request& req, response& resp);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
