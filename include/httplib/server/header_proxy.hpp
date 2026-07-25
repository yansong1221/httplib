#pragma once
#include "httplib/config.hpp"
#include <boost/beast/http/fields.hpp>
#include <string>
#include <string_view>

namespace httplib::server {

class header_proxy
{
public:
    header_proxy(http::fields& fields, std::string_view name)
        : fields_(fields)
        , name_(name)
    {
    }

    header_proxy(http::fields& fields, http::field name)
        : fields_(fields)
        , name_(http::to_string(name))
    {
    }

    operator std::string_view() const { return fields_[name_]; }

    header_proxy& operator=(std::string_view value)
    {
        fields_.set(name_, value);
        return *this;
    }

    header_proxy& operator=(const std::string& value)
    {
        fields_.set(name_, std::string_view(value));
        return *this;
    }

private:
    http::fields& fields_;
    std::string name_;
};

} // namespace httplib::server
