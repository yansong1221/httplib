#pragma once
#include "httplib/config.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <functional>
#include <string>
#include <string_view>

namespace httplib::server::middleware {

namespace detail {

inline std::string base64_decode(std::string_view encoded)
{
    static constexpr std::string_view table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string decoded;
    int val = 0, bits = -8;
    for (unsigned char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r')
            break;
        auto pos = table.find(c);
        if (pos == std::string_view::npos)
            continue;
        val = (val << 6) | static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return decoded;
}

} // namespace detail

class basic_auth_middleware
{
public:
    using validator_t = std::function<bool(std::string_view username, std::string_view password)>;

    explicit basic_auth_middleware(validator_t validator, std::string realm = "Restricted")
        : validator_(std::move(validator))
        , realm_(std::move(realm))
    {
    }

    bool before(request& req, response& resp)
    {
        auto auth = std::string(req.base()[http::field::authorization]);
        if (auth.starts_with("Basic ")) {
            auto creds      = detail::base64_decode(std::string_view(auth).substr(6));
            auto colon      = creds.find(':');
            auto username   = std::string_view(creds).substr(0, colon);
            auto password   = colon != std::string::npos
                                  ? std::string_view(creds).substr(colon + 1)
                                  : std::string_view{};
            if (validator_ && validator_(username, password))
                return true;
        }

        resp.set(http::field::www_authenticate,
                 std::string("Basic realm=\"") + realm_ + '"');
        resp.set_json_content({{"error", "unauthorized"}}, http::status::unauthorized);
        return false;
    }

private:
    validator_t validator_;
    std::string realm_;
};

class bearer_auth_middleware
{
public:
    using validator_t = std::function<bool(std::string_view token)>;

    explicit bearer_auth_middleware(validator_t validator, std::string realm = "Restricted")
        : validator_(std::move(validator))
        , realm_(std::move(realm))
    {
    }

    bool before(request& req, response& resp)
    {
        auto auth = std::string(req.base()[http::field::authorization]);
        if (auth.starts_with("Bearer ")) {
            auto token = std::string_view(auth).substr(7);
            if (validator_ && validator_(token))
                return true;
        }

        resp.set(http::field::www_authenticate,
                 std::string("Bearer realm=\"") + realm_ + '"');
        resp.set_json_content({{"error", "unauthorized"}}, http::status::unauthorized);
        return false;
    }

private:
    validator_t validator_;
    std::string realm_;
};

} // namespace httplib::server::middleware
