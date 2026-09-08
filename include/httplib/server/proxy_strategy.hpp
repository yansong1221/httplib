#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <string>
#include <utility>

namespace httplib::server
{

    /**
     * \brief Public configuration for a single upstream backend.
     *
     * The backend's weight (when using \c weighted_round_robin) and its order in
     * the list matter. Runtime state (health, active connections) is internal.
     */
    struct upstream_backend
    {
        std::string url;
        uint32_t weight = 1;

        upstream_backend() = default;
        upstream_backend(std::string u, uint32_t w = 1) : url(std::move(u)), weight(w) {}
    };

    /**
     * \brief Backend selection algorithm used when resolving an upstream URL.
     */
    enum class upstream_locator
    {
        round_robin,
        weighted_round_robin,
        least_connections
    };

    /**
     * \brief Provides the upstream URL a proxied request is forwarded to.
     *
     * Implement this interface and pass an instance directly to
     * \c set_reverse_proxy / \c set_ws_forward. Static URLs and load-balanced
     * backend groups are covered by the string / backend-list overloads instead.
     */
    class upstream_provider
    {
      public:
        virtual ~upstream_provider() = default;

        /// \brief Resolve the upstream URL for a single request.
        /// reverse proxy expects http(s)://, ws forward expects ws(s)://.
        virtual net::awaitable<std::string> url(request& req) = 0;
    };

} // namespace httplib::server
