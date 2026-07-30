#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::server::middleware {

class HTTPLIB_API cors_middleware
{
public:
    cors_middleware();
    ~cors_middleware();
    cors_middleware(const cors_middleware& other);
    cors_middleware& operator=(const cors_middleware& other);
    cors_middleware(cors_middleware&&) noexcept;
    cors_middleware& operator=(cors_middleware&&) noexcept;

    cors_middleware& allow_origin(std::string origin);
    cors_middleware& allow_origins(std::vector<std::string> origins);
    cors_middleware& allow_methods(std::vector<std::string> methods);
    cors_middleware& allow_headers(std::vector<std::string> headers);
    cors_middleware& allow_credentials(bool allow);
    cors_middleware& max_age(int seconds);

    bool before(request& req, response& resp);
    bool after(request& req, response& resp);

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httplib::server::middleware
