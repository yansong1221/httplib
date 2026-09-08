#pragma once
#include "httplib/server/proxy_strategy.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib::server
{

    /// An upstream backend with runtime health/load state (internal only).
    struct group_backend : upstream_backend
    {
        std::atomic<size_t> active { 0 };
        std::atomic<bool> healthy { true };

        group_backend() = default;
        explicit group_backend(upstream_backend const& cfg);

        group_backend(group_backend const& other);
        group_backend& operator=(group_backend const& other);
        group_backend(group_backend&& other) noexcept;
        group_backend& operator=(group_backend&& other) noexcept;
    };

    /**
     * \brief Thread-safe collection of upstream backends shared across requests.
     * \details Also acts as an upstream_provider: selecting a backend via the
     * configured locator and yielding its URL for each request.
     */
    class upstream_group
        : public upstream_provider
        , public std::enable_shared_from_this<upstream_group>
    {
      public:
        explicit upstream_group(std::vector<group_backend> backends,
                                upstream_locator locator = upstream_locator::round_robin);

        size_t size() const;

        group_backend& at(size_t i);
        group_backend const& at(size_t i) const;

        net::awaitable<std::string> url(request&) override;

        /// \brief Track an in-flight request against the backend that yielded \p url.
        void on_acquired(std::string const& url) override;

        /// \brief Release the in-flight request for the backend that yielded \p url.
        void on_released(std::string const& url) override;

        /// Pick the next backend per the locator and return its URL (synchronous).
        /// Throws when there are no healthy backends.
        std::string const& resolve_url();

        /// Return all healthy backends.
        std::vector<group_backend*> healthy_backends();

        void mark_unhealthy(size_t idx);
        void mark_healthy(size_t idx);

      private:
        std::vector<group_backend> backends_;
        upstream_locator locator_;
        std::atomic<size_t> rr_index_ { 0 };

        /// backend url -> index; built once in the constructor and read-only after,
        /// so concurrent lookups need no lock.
        std::unordered_map<std::string, size_t> url_index_;

        mutable std::mutex mu_;
        std::vector<std::ptrdiff_t> current_;

        /// Round-robin: atomic increment, skip unhealthy.
        group_backend& next_rr();

        /// Weighted round-robin (Nginx smooth WRR): returns the selected backend.
        group_backend& next_weighted();

        /// Least active connections.
        group_backend& least_conn();

        group_backend& do_resolve();
    };

    std::vector<group_backend> make_backends(std::vector<upstream_backend> const& cfg);

} // namespace httplib::server
