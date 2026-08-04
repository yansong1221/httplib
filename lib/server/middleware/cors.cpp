#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <algorithm>

namespace httplib::server::middleware
{

    class cors::impl
    {
      public:
        std::vector<std::string> origins_ { "*" };
        std::vector<std::string> methods_ { "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS" };
        std::vector<std::string> headers_ { "Origin", "Content-Type", "Accept", "Authorization" };
        bool credentials_ = false;
        int max_age_ = 0;

        static std::string
        join(std::string_view sep, std::vector<std::string> const& parts)
        {
            std::string result;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i > 0)
                {
                    result += sep;
                }
                result += parts[i];
            }
            return result;
        }

        std::string
        origin_header() const
        {
            if (origins_.empty())
            {
                return "*";
            }
            if (origins_.size() == 1)
            {
                return origins_[0];
            }
            return {};
        }

        void
        apply(response& resp) const
        {
            auto origin = origin_header();
            if (!origin.empty())
            {
                resp.set(std::string_view("Access-Control-Allow-Origin"), origin);
            }
            if (!methods_.empty())
            {
                resp.set(std::string_view("Access-Control-Allow-Methods"), join(std::string_view(", "), methods_));
            }
            if (!headers_.empty())
            {
                resp.set(std::string_view("Access-Control-Allow-Headers"), join(std::string_view(", "), headers_));
            }
            if (credentials_)
            {
                resp.set(std::string_view("Access-Control-Allow-Credentials"), std::string_view("true"));
            }
            if (max_age_ > 0)
            {
                resp.set(std::string_view("Access-Control-Max-Age"), std::to_string(max_age_));
            }
        }
    };

    cors::cors() : impl_(std::make_unique<impl>()) {}
    cors::~cors() = default;

    cors::cors(cors const& other) : impl_(std::make_unique<impl>(*other.impl_)) {}

    cors&
    cors::operator=(cors const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(*other.impl_);
        }
        return *this;
    }

    cors::cors(cors&&) noexcept = default;
    cors& cors::operator=(cors&&) noexcept = default;

    cors&
    cors::allow_origin(std::string origin)
    {
        impl_->origins_.clear();
        impl_->origins_.push_back(std::move(origin));
        return *this;
    }

    cors&
    cors::allow_origins(std::vector<std::string> origins)
    {
        impl_->origins_ = std::move(origins);
        return *this;
    }

    cors&
    cors::allow_methods(std::vector<std::string> methods)
    {
        impl_->methods_ = std::move(methods);
        return *this;
    }

    cors&
    cors::allow_headers(std::vector<std::string> headers)
    {
        impl_->headers_ = std::move(headers);
        return *this;
    }

    cors&
    cors::allow_credentials(bool allow)
    {
        impl_->credentials_ = allow;
        return *this;
    }

    cors&
    cors::max_age(int seconds)
    {
        impl_->max_age_ = seconds;
        return *this;
    }

    bool
    cors::before(request& req, response& resp)
    {
        if (req.method() == http::verb::options)
        {
            impl_->apply(resp);
            resp.set_empty_content(http::status::no_content);
            return false;
        }
        return true;
    }

    bool
    cors::after(request&, response& resp)
    {
        impl_->apply(resp);
        return true;
    }

} // namespace httplib::server::middleware
