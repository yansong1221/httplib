#include "upstream_group.hpp"
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace httplib::server
{

    group_backend::group_backend(upstream_backend const& cfg) : url(cfg.url), weight(cfg.weight) {}

    group_backend::group_backend(group_backend const& other)
        : url(other.url)
        , weight(other.weight)
        , active(other.active.load())
        , healthy(other.healthy.load())
    {
    }

    group_backend&
    group_backend::operator=(group_backend const& other)
    {
        if (this != &other)
        {
            url = other.url;
            weight = other.weight;
            active.store(other.active.load());
            healthy.store(other.healthy.load());
        }
        return *this;
    }

    group_backend::group_backend(group_backend&& other) noexcept
        : url(std::move(other.url))
        , weight(other.weight)
        , active(other.active.load())
        , healthy(other.healthy.load())
    {
    }

    group_backend&
    group_backend::operator=(group_backend&& other) noexcept
    {
        if (this != &other)
        {
            url = std::move(other.url);
            weight = other.weight;
            active.store(other.active.load());
            healthy.store(other.healthy.load());
        }
        return *this;
    }

    upstream_group::upstream_group(std::vector<group_backend> backends) : backends_(std::move(backends)) {}

    size_t
    upstream_group::size() const
    {
        return backends_.size();
    }

    group_backend&
    upstream_group::at(size_t i)
    {
        return backends_[i];
    }

    group_backend const&
    upstream_group::at(size_t i) const
    {
        return backends_[i];
    }

    std::string
    upstream_group::resolve(upstream_locator locator)
    {
        switch (locator)
        {
            case upstream_locator::weighted_round_robin:
                return next_weighted().url;
            case upstream_locator::least_connections:
                return least_conn().url;
            case upstream_locator::round_robin:
            default:
                return next_rr().url;
        }
    }

    std::vector<group_backend*>
    upstream_group::healthy_backends()
    {
        std::vector<group_backend*> result;
        for (auto& b : backends_)
        {
            if (b.healthy.load(std::memory_order_acquire))
            {
                result.push_back(&b);
            }
        }
        return result;
    }

    void
    upstream_group::mark_unhealthy(size_t idx)
    {
        if (idx < backends_.size())
        {
            backends_[idx].healthy.store(false, std::memory_order_release);
        }
    }

    void
    upstream_group::mark_healthy(size_t idx)
    {
        if (idx < backends_.size())
        {
            backends_[idx].healthy.store(true, std::memory_order_release);
        }
    }

    group_backend&
    upstream_group::next_rr()
    {
        if (backends_.empty())
        {
            throw std::runtime_error("upstream_group: no backends");
        }
        auto n = backends_.size();
        for (size_t i = 0; i < n; ++i)
        {
            auto idx = rr_index_.fetch_add(1, std::memory_order_relaxed) % n;
            if (backends_[idx].healthy.load(std::memory_order_acquire))
            {
                return backends_[idx];
            }
        }
        throw std::runtime_error("upstream_group: no healthy backends");
    }

    group_backend&
    upstream_group::next_weighted()
    {
        if (backends_.empty())
        {
            throw std::runtime_error("upstream_group: no backends");
        }
        auto n = backends_.size();

        std::lock_guard lk(mu_);
        if (current_.size() != n)
        {
            current_.assign(n, 0);
        }

        std::ptrdiff_t total_weight = 0;
        std::ptrdiff_t best = std::numeric_limits<std::ptrdiff_t>::min();
        std::ptrdiff_t selected = -1;
        for (size_t i = 0; i < n; ++i)
        {
            bool healthy = backends_[i].healthy.load(std::memory_order_acquire);
            std::ptrdiff_t w = healthy ? static_cast<std::ptrdiff_t>(backends_[i].weight) : 0;
            total_weight += w;
            if (!healthy)
            {
                continue;
            }
            current_[i] += w;
            if (current_[i] > best)
            {
                best = current_[i];
                selected = static_cast<std::ptrdiff_t>(i);
            }
        }

        if (selected < 0)
        {
            throw std::runtime_error("upstream_group: no healthy backends");
        }

        current_[selected] -= total_weight;
        return backends_[selected];
    }

    group_backend&
    upstream_group::least_conn()
    {
        if (backends_.empty())
        {
            throw std::runtime_error("upstream_group: no backends");
        }
        group_backend* best = nullptr;
        for (auto& b : backends_)
        {
            if (!b.healthy.load(std::memory_order_acquire))
            {
                continue;
            }
            if (!best || b.active.load(std::memory_order_relaxed) < best->active.load(std::memory_order_relaxed))
            {
                best = &b;
            }
        }
        if (!best)
        {
            throw std::runtime_error("upstream_group: no healthy backends");
        }
        return *best;
    }

    std::vector<group_backend>
    make_backends(std::vector<upstream_backend> const& cfg)
    {
        std::vector<group_backend> result;
        result.reserve(cfg.size());
        for (auto const& b : cfg)
        {
            result.emplace_back(b);
        }
        return result;
    }

} // namespace httplib::server
