#include "httplib/server/middleware/cors.hpp"
#include <algorithm>

namespace httplib::server::middleware {

class cors_middleware::impl
{
public:
    std::vector<std::string> origins_{"*"};
    std::vector<std::string> methods_{
        "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
    std::vector<std::string> headers_{
        "Origin", "Content-Type", "Accept", "Authorization"};
    bool credentials_ = false;
    int max_age_      = 0;

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

    std::string origin_header() const
    {
        if (origins_.empty())
            return "*";
        if (origins_.size() == 1)
            return origins_[0];
        return {};
    }

    void apply(response& resp) const
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
};

cors_middleware::cors_middleware() : impl_(std::make_unique<impl>()) {}
cors_middleware::~cors_middleware() = default;

cors_middleware::cors_middleware(const cors_middleware& other)
    : impl_(std::make_unique<impl>(*other.impl_))
{
}

cors_middleware& cors_middleware::operator=(const cors_middleware& other)
{
    if (this != &other)
        impl_ = std::make_unique<impl>(*other.impl_);
    return *this;
}

cors_middleware::cors_middleware(cors_middleware&&) noexcept            = default;
cors_middleware& cors_middleware::operator=(cors_middleware&&) noexcept = default;

cors_middleware& cors_middleware::allow_origin(std::string origin)
{
    impl_->origins_.clear();
    impl_->origins_.push_back(std::move(origin));
    return *this;
}

cors_middleware& cors_middleware::allow_origins(std::vector<std::string> origins)
{
    impl_->origins_ = std::move(origins);
    return *this;
}

cors_middleware& cors_middleware::allow_methods(std::vector<std::string> methods)
{
    impl_->methods_ = std::move(methods);
    return *this;
}

cors_middleware& cors_middleware::allow_headers(std::vector<std::string> headers)
{
    impl_->headers_ = std::move(headers);
    return *this;
}

cors_middleware& cors_middleware::allow_credentials(bool allow)
{
    impl_->credentials_ = allow;
    return *this;
}

cors_middleware& cors_middleware::max_age(int seconds)
{
    impl_->max_age_ = seconds;
    return *this;
}

bool cors_middleware::before(request& req, response& resp)
{
    if (req.method() == http::verb::options) {
        impl_->apply(resp);
        resp.set_empty_content(http::status::no_content);
        return false;
    }
    return true;
}

bool cors_middleware::after(request&, response& resp)
{
    impl_->apply(resp);
    return true;
}

} // namespace httplib::server::middleware
