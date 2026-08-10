#pragma once
#include "httplib/server/request.hpp"
#include <any>

namespace httplib::server::middleware
{

    template <typename MW>
    auto&
    fetch(request& req, std::string_view tag = {})
    {
        return req.data().template fetch<typename MW::value_type>(tag);
    }

    template <typename MW>
    auto const&
    fetch(request const& req, std::string_view tag = {})
    {
        return req.data().template fetch<typename MW::value_type>();
    }

    template <typename MW>
    bool
    has(request const& req, std::string_view tag = {})
    {
        return req.data().template has<typename MW::value_type>(tag);
    }

    template <typename MW>
    void
    store(request& req, std::any val, std::string_view tag = {})
    {
        req.data().template store<typename MW::value_type>(
            tag, std::any_cast<typename MW::value_type>(std::move(val)));
    }

    template <typename MW>
    void
    erase(request& req, std::string_view tag = {})
    {
        req.data().erase<typename MW::value_type>(tag);
    }

} // namespace httplib::server::middleware
