#pragma once
#include "httplib/config.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace httplib::html {


struct cookie
{
    enum class same_site_t
    {
        none,
        lax,
        strict
    };

    std::string name;
    std::string value;
    std::string path {"/"};
    std::string domain;
    std::chrono::seconds max_age {0};
    bool secure           = false;
    bool http_only        = true;
    same_site_t same_site = same_site_t::lax;

    std::string to_set_cookie_string() const
    {
        std::string s = std::format("{}={}", name, value);
        if (!path.empty())
            s += std::format("; Path={}", path);
        if (!domain.empty())
            s += std::format("; Domain={}", domain);
        if (max_age.count() > 0)
            s += std::format("; Max-Age={}", max_age.count());
        if (secure)
            s += "; Secure";
        if (http_only)
            s += "; HttpOnly";
        switch (same_site) {
            case same_site_t::none: s += "; SameSite=None"; break;
            case same_site_t::lax: s += "; SameSite=Lax"; break;
            case same_site_t::strict: s += "; SameSite=Strict"; break;
        }
        return s;
    }
};

class cookie_jar
{
public:
    cookie_jar() : impl_(std::make_unique<impl>()) {}
    ~cookie_jar() = default;

    cookie_jar(const cookie_jar& other)
        : impl_(std::make_unique<impl>(*other.impl_))
    {
    }

    cookie_jar& operator=(const cookie_jar& other)
    {
        if (this != &other)
            impl_ = std::make_unique<impl>(*other.impl_);
        return *this;
    }

    cookie_jar(cookie_jar&&) noexcept            = default;
    cookie_jar& operator=(cookie_jar&&) noexcept = default;

    static cookie_jar parse(std::string_view header)
    {
        cookie_jar jar;
        while (!header.empty()) {
            auto semi = header.find(';');
            auto part = semi == std::string_view::npos ? header : header.substr(0, semi);

            auto eq = part.find('=');
            if (eq != std::string_view::npos) {
                auto name  = impl::trim(part.substr(0, eq));
                auto value = impl::trim(part.substr(eq + 1));
                jar.impl_->values_[std::string(name)] = std::string(value);
                jar.impl_->cookies_.push_back(
                    cookie{.name = std::string(name), .value = std::string(value)});
            }

            if (semi == std::string_view::npos)
                break;
            header = header.substr(semi + 1);
        }
        return jar;
    }

    std::optional<std::string> get(std::string_view name) const
    {
        auto it = impl_->values_.find(std::string(name));
        if (it != impl_->values_.end())
            return it->second;
        return std::nullopt;
    }

    bool has(std::string_view name) const
    {
        return impl_->values_.count(std::string(name)) > 0;
    }

    size_t size() const
    {
        return impl_->values_.size();
    }

    const std::unordered_map<std::string, std::string>& values() const
    {
        return impl_->values_;
    }

    const std::vector<cookie>& all() const
    {
        return impl_->cookies_;
    }

private:
    struct impl
    {
        std::unordered_map<std::string, std::string> values_;
        std::vector<cookie> cookies_;

        static std::string_view trim(std::string_view s)
        {
            auto start = s.find_first_not_of(' ');
            if (start == std::string_view::npos)
                return {};
            auto end = s.find_last_not_of(' ');
            return s.substr(start, end - start + 1);
        }
    };

    std::unique_ptr<impl> impl_;
};

} // namespace httplib::html
