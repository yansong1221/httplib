#include "cookie_jar.hpp"

namespace httplib::html {

class cookie_jar::impl
{
public:
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

cookie_jar::cookie_jar() : impl_(std::make_unique<impl>()) {}
cookie_jar::~cookie_jar() = default;

cookie_jar::cookie_jar(const cookie_jar& other)
    : impl_(std::make_unique<impl>(*other.impl_))
{
}

cookie_jar& cookie_jar::operator=(const cookie_jar& other)
{
    if (this != &other)
        impl_ = std::make_unique<impl>(*other.impl_);
    return *this;
}

cookie_jar::cookie_jar(cookie_jar&&) noexcept            = default;
cookie_jar& cookie_jar::operator=(cookie_jar&&) noexcept = default;

cookie_jar cookie_jar::parse(std::string_view header)
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

std::optional<std::string> cookie_jar::get(std::string_view name) const
{
    auto it = impl_->values_.find(std::string(name));
    if (it != impl_->values_.end())
        return it->second;
    return std::nullopt;
}

bool cookie_jar::has(std::string_view name) const
{
    return impl_->values_.count(std::string(name)) > 0;
}

size_t cookie_jar::size() const
{
    return impl_->values_.size();
}

const std::unordered_map<std::string, std::string>& cookie_jar::values() const
{
    return impl_->values_;
}

const std::vector<cookie>& cookie_jar::all() const
{
    return impl_->cookies_;
}

} // namespace httplib::html
