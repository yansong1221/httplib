#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
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

        /// \brief Acquire an opaque ticket identifying the backend selected for
        /// this request.
        /// \details The ticket is acquired at selection time, before the
        /// connection is established, so connect/handshake failures can be
        /// reported against the same backend. A non-zero return value switches
        /// the request to ticket-based accounting: release_ticket(),
        /// report_success() and report_failure() are used instead of the legacy
        /// on_acquired()/on_released() pair. Returning 0 keeps the legacy path.
        /// The default implementation returns 0.
        virtual std::uint64_t
        acquire_ticket(std::string const& url)
        {
            (void)url;
            return 0;
        }

        /// \brief Release a ticket previously returned by acquire_ticket().
        /// \details Must be called exactly once for every non-zero ticket.
        virtual void
        release_ticket(std::uint64_t ticket) noexcept
        {
            (void)ticket;
        }

        /// \brief Report that the upstream response was received successfully.
        /// \details Called after the response headers arrive (HTTP) or after the
        /// WebSocket handshake succeeds. Only called for ticket-based providers.
        virtual void
        report_success(std::uint64_t ticket) noexcept
        {
            (void)ticket;
        }

        /// \brief Report an upstream transport failure for a ticket.
        /// \details Only called for ticket-based providers and only when the
        /// error is attributable to the upstream (cancellation is filtered out
        /// by the caller). \p ec carries the transport error.
        virtual void
        report_failure(std::uint64_t ticket, boost::system::error_code const& ec) noexcept
        {
            (void)ticket;
            (void)ec;
        }

        /// \brief Legacy hook used when acquire_ticket() returns 0.
        /// \details Called once the upstream connection for this request was
        /// acquired. \p url is the exact string previously returned by url().
        /// The default implementation does nothing; upstream_group overrides it to
        /// track in-flight load for least_connections.
        virtual void
        on_acquired(std::string const& url)
        {
            (void)url;
        }

        /// \brief Legacy hook used when acquire_ticket() returns 0.
        /// \details Called when the request's upstream connection is released/closed.
        /// Must pair with on_acquired(); guarded by the implementation to fire once.
        virtual void
        on_released(std::string const& url)
        {
            (void)url;
        }
    };

} // namespace httplib::server
