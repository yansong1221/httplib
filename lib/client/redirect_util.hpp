#pragma once
#include "httplib/config.hpp"
#include <boost/beast/http/fields.hpp>

namespace httplib::client::redirect
{

    /// Remove origin-bound credentials before following a cross-origin redirect,
    /// so they cannot leak to a different host.
    inline void
    strip_origin_bound_headers(http::fields& headers)
    {
        headers.erase(http::field::authorization);
        headers.erase(http::field::proxy_authorization);
        headers.erase(http::field::cookie);
        headers.erase(http::field::cookie2);
    }

} // namespace httplib::client::redirect
