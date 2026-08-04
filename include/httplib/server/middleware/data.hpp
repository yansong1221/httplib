#pragma once
#include "httplib/server/request.hpp"
#include <any>

namespace httplib::server::middleware
{

    template <typename MW>
    auto&
    get_data(request& req)
    {
        return std::any_cast<typename MW::value_type&>(req.custom_data(MW::key));
    }

    template <typename MW>
    auto const&
    get_data(request const& req)
    {
        return std::any_cast<typename MW::value_type const&>(req.custom_data(MW::key));
    }

} // namespace httplib::server::middleware
