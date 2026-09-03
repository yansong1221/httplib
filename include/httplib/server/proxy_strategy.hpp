#pragma once
#include "httplib/config.hpp"
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

} // namespace httplib::server
