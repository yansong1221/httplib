#pragma once
#include "httplib/config.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::server::middleware {

class cors_middleware
{
public:
    cors_middleware& allow_origin(std::string origin)
    {
        origins_.clear();
        origins_.push_back(std::move(origin));
        return *this;
    }

    cors_middleware& allow_origins(std::vector<std::string> origins)
    {
        origins_ = std::move(origins);
        return *this;
    }

    cors_middleware& allow_methods(std::vector<std::string> methods)
    {
        methods_ = std::move(methods);
        return *this;
    }

    cors_middleware& allow_headers(std::vector<std::string> headers)
    {
        headers_ = std::move(headers);
        return *this;
    }

    cors_middleware& allow_credentials(bool allow)
    {
        credentials_ = allow;
        return *this;
    }

    cors_middleware& max_age(int seconds)
    {
        max_age_ = seconds;
        return *this;
    }

    bool before(request& req, response& resp)
    {
        if (req.method() == http::verb::options) {
            apply_cors_headers(resp);
            resp.set_empty_content(http::status::no_content);
            return false;
        }
        return true;
    }

    bool after(request&, response& resp)
    {
        apply_cors_headers(resp);
        return true;
    }

private:
    static std::string join(std::string_view sep, const std::vector<std::string>& parts)
    {
        std::string result;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0)
                result += sep;
            result += parts[i];
        }
        return result;
    }

    void apply_cors_headers(response& resp) const
    {
        auto origin = origin_header();
        if (!origin.empty())
            resp.set(std::string_view("Access-Control-Allow-Origin"), origin);
        if (!methods_.empty())
            resp.set(std::string_view("Access-Control-Allow-Methods"),
                     join(std::string_view(", "), methods_));
        if (!headers_.empty())
            resp.set(std::string_view("Access-Control-Allow-Headers"),
                     join(std::string_view(", "), headers_));
        if (credentials_)
            resp.set(std::string_view("Access-Control-Allow-Credentials"),
                     std::string_view("true"));
        if (max_age_ > 0)
            resp.set(std::string_view("Access-Control-Max-Age"), std::to_string(max_age_));
    }

    std::string origin_header() const
    {
        if (origins_.empty())
            return "*";
        if (origins_.size() == 1)
            return origins_[0];
        return {};
    }

    std::vector<std::string> origins_{"*"};
    std::vector<std::string> methods_{
        "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
    std::vector<std::string> headers_{
        "Origin", "Content-Type", "Accept", "Authorization"};
    bool credentials_ = false;
    int max_age_      = 0;
};

} // namespace httplib::server::middleware
