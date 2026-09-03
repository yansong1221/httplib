#include "upstream_group.hpp"
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace httplib::server
{

    class upstream_proxy_target final : public http_server::proxy_target
    {
      public:
        upstream_proxy_target(std::string url, std::shared_ptr<upstream_group> group, size_t idx)
            : url_(std::move(url))
            , group_(std::move(group))
            , idx_(idx)
        {
            group_->active_inc(idx_);
        }

        ~upstream_proxy_target() override
        {
            group_->active_dec(idx_);
        }

        std::string const& url() const override
        {
            return url_;
        }

      private:
        std::string url_;
        std::shared_ptr<upstream_group> group_;
        size_t idx_;
    };

    group_backend::group_backend(upstream_backend const& cfg) : upstream_backend(cfg) {}

    group_backend::group_backend(group_backend const& other)
        : upstream_backend(other)
        , active(other.active.load())
        , healthy(other.healthy.load())
    {
    }

    group_backend&
    group_backend::operator=(group_backend const& other)
    {
        if (this != &other)
        {
            upstream_backend::operator=(other);
            active.store(other.active.load());
            healthy.store(other.healthy.load());
        }
        return *this;
    }

    group_backend::group_backend(group_backend&& other) noexcept
        : upstream_backend(std::move(other))
        , active(other.active.load())
        , healthy(other.healthy.load())
    {
    }

    group_backend&
    group_backend::operator=(group_backend&& other) noexcept
    {
        if (this != &other)
        {
            upstream_backend::operator=(std::move(other));
            active.store(other.active.load());
            healthy.store(other.healthy.load());
        }
        return *this;
    }

    upstream_group::upstream_group(std::vector<group_backend> backends, upstream_locator locator)
        : backends_(std::move(backends))
        , locator_(locator)
    {
    }

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

    group_backend&
    upstream_group::do_resolve()
    {
        switch (locator_)
        {
            case upstream_locator::weighted_round_robin:
                return next_weighted();
            case upstream_locator::least_connections:
                return least_conn();
            case upstream_locator::round_robin:
            default:
                return next_rr();
        }
    }

    std::shared_ptr<http_server::proxy_target>
    upstream_group::resolve_target()
    {
        auto& b = do_resolve();
        auto idx = &b - &backends_[0];
        return std::make_shared<upstream_proxy_target>(b.url, shared_from_this(), idx);
    }

    void
    upstream_group::active_inc(size_t idx)
    {
        if (idx < backends_.size())
        {
            backends_[idx].active.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void
    upstream_group::active_dec(size_t idx)
    {
        if (idx < backends_.size())
        {
            backends_[idx].active.fetch_sub(1, std::memory_order_relaxed);
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
