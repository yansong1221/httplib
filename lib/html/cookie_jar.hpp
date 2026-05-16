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
    cookie_jar();
    ~cookie_jar();
    cookie_jar(const cookie_jar&);
    cookie_jar& operator=(const cookie_jar&);
    cookie_jar(cookie_jar&&) noexcept;
    cookie_jar& operator=(cookie_jar&&) noexcept;

    static cookie_jar parse(std::string_view header);

    std::optional<std::string> get(std::string_view name) const;
    bool has(std::string_view name) const;
    size_t size() const;
    const std::unordered_map<std::string, std::string>& values() const;
    const std::vector<cookie>& all() const;

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httplib::html
