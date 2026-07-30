#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::server::middleware {

class HTTPLIB_API basic_auth_middleware
{
public:
    using validator_t = std::function<bool(std::string_view username, std::string_view password)>;

    explicit basic_auth_middleware(validator_t validator, std::string realm = "Restricted");
    ~basic_auth_middleware();
    basic_auth_middleware(const basic_auth_middleware&);
    basic_auth_middleware& operator=(const basic_auth_middleware&);
    basic_auth_middleware(basic_auth_middleware&&) noexcept;
    basic_auth_middleware& operator=(basic_auth_middleware&&) noexcept;

    bool before(request& req, response& resp);

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

class HTTPLIB_API bearer_auth_middleware
{
public:
    using validator_t = std::function<bool(std::string_view token)>;

    explicit bearer_auth_middleware(validator_t validator, std::string realm = "Restricted");
    ~bearer_auth_middleware();
    bearer_auth_middleware(const bearer_auth_middleware&);
    bearer_auth_middleware& operator=(const bearer_auth_middleware&);
    bearer_auth_middleware(bearer_auth_middleware&&) noexcept;
    bearer_auth_middleware& operator=(bearer_auth_middleware&&) noexcept;

    bool before(request& req, response& resp);

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httplib::server::middleware
