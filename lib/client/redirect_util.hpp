#pragma once
#include "httplib/config.hpp"
#include "httplib/headers.hpp"

namespace httplib::client::redirect
{

    /// Remove origin-bound credentials before following a cross-origin redirect,
    /// so they cannot leak to a different host.
    inline void
    strip_origin_bound_headers(httplib::headers& headers)
    {
        headers.erase(field::authorization);
        headers.erase(field::proxy_authorization);
        headers.erase(field::cookie);
        headers.erase(field::cookie2);
    }

} // namespace httplib::client::redirect
