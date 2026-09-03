#pragma once
#include "httplib/server/proxy_strategy.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace httplib::server
{

    /// An upstream backend with runtime health/load state (internal only).
    struct group_backend
    {
        std::string url;
        uint32_t weight = 1;
        std::atomic<size_t> active { 0 };
        std::atomic<bool> healthy { true };

        explicit group_backend(upstream_backend const& cfg);

        group_backend() = default;
        group_backend(group_backend const& other);
        group_backend& operator=(group_backend const& other);
        group_backend(group_backend&& other) noexcept;
        group_backend& operator=(group_backend&& other) noexcept;
    };

    /**
     * \brief Thread-safe collection of upstream backends shared across requests.
     */
    class upstream_group : public std::enable_shared_from_this<upstream_group>
    {
      public:
        explicit upstream_group(std::vector<group_backend> backends);

        size_t size() const;

        group_backend& at(size_t i);
        group_backend const& at(size_t i) const;

        /// Resolve the next upstream URL using the given selection algorithm.
        std::string resolve(upstream_locator locator = upstream_locator::round_robin);

        /// Return all healthy backends.
        std::vector<group_backend*> healthy_backends();

        void mark_unhealthy(size_t idx);
        void mark_healthy(size_t idx);

      private:
        std::vector<group_backend> backends_;
        std::atomic<size_t> rr_index_ { 0 };

        mutable std::mutex mu_;
        std::vector<std::ptrdiff_t> current_;

        /// Round-robin: atomic increment, skip unhealthy.
        group_backend& next_rr();

        /// Weighted round-robin (Nginx smooth WRR): returns the selected backend.
        group_backend& next_weighted();

        /// Least active connections.
        group_backend& least_conn();
    };

    std::vector<group_backend> make_backends(std::vector<upstream_backend> const& cfg);

} // namespace httplib::server
