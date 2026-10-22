#pragma once
#include "httplib/server/proxy_strategy.hpp"
#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace httplib::server::detail
{
    /// \brief Whether an upstream transport error should mark the backend unhealthy.
    /// \details Cancellation is usually caused by the downstream client going
    /// away or by server shutdown; it says nothing about the upstream backend.
    inline bool
    is_upstream_failure(boost::system::error_code const& ec)
    {
        if (!ec)
        {
            return false;
        }
        if (ec == net::error::operation_aborted)
        {
            return false;
        }
        if (ec == boost::system::errc::make_error_code(boost::system::errc::operation_canceled))
        {
            return false;
        }
        return true;
    }

    /**
     * \brief RAII lease for an upstream provider.
     * \details A provider that implements acquire_ticket() returns a non-zero
     * opaque ticket. Accounting and health feedback then use the ticket, so a
     * provider returning URL variants can no longer break the release pairing.
     * Providers that keep the default (ticket 0) fall back to the legacy
     * on_acquired()/on_released() string pair at the original timing.
     */
    class upstream_lease
    {
      public:
        upstream_lease(std::shared_ptr<upstream_provider> provider, std::string url)
            : provider_(std::move(provider))
            , url_(std::move(url))
        {
        }

        ~upstream_lease()
        {
            if (!provider_)
            {
                return;
            }
            if (ticket_ != 0)
            {
                provider_->release_ticket(ticket_);
            }
            else if (legacy_started_)
            {
                provider_->on_released(url_);
            }
        }

        upstream_lease(upstream_lease const&) = delete;
        upstream_lease& operator=(upstream_lease const&) = delete;

        /// \brief Try ticket-based accounting; a zero ticket selects the legacy path.
        /// \details Called immediately after the provider resolved the URL, so a
        /// connect/handshake failure can still be reported for the selected backend.
        void
        begin()
        {
            if (started_ || !provider_)
            {
                return;
            }
            started_ = true;
            ticket_ = provider_->acquire_ticket(url_);
        }

        /// \brief Start the legacy on_acquired() hook at the old point in the request.
        /// \details Ticket-based providers ignore this. For legacy providers it is
        /// called after the connection (or WebSocket handshake) was acquired.
        void
        start_legacy()
        {
            if (legacy_started_ || ticket_ != 0 || !provider_)
            {
                return;
            }
            if (!started_)
            {
                begin();
            }
            if (ticket_ == 0)
            {
                legacy_started_ = true;
                provider_->on_acquired(url_);
            }
        }

        /// \brief Report a successful upstream exchange for ticket-based providers.
        void
        success() noexcept
        {
            if (ticket_ != 0 && provider_ && !success_reported_)
            {
                success_reported_ = true;
                provider_->report_success(ticket_);
            }
        }

        /// \brief Report an upstream transport failure for ticket-based providers.
        /// \details Cancellation is filtered out in is_upstream_failure().
        void
        failure(boost::system::error_code const& ec) noexcept
        {
            if (ticket_ != 0 && provider_ && !failure_reported_ && is_upstream_failure(ec))
            {
                failure_reported_ = true;
                provider_->report_failure(ticket_, ec);
            }
        }

      private:
        std::shared_ptr<upstream_provider> provider_;
        std::string url_;
        std::uint64_t ticket_ = 0;
        bool started_ = false;
        bool legacy_started_ = false;
        bool success_reported_ = false;
        bool failure_reported_ = false;
    };
} // namespace httplib::server::detail
