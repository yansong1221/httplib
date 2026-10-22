#include "upstream_group.hpp"
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace httplib::server
{

    group_backend::group_backend(upstream_backend const& cfg) : upstream_backend(cfg) {}

    group_backend::group_backend(group_backend const& other)
        : upstream_backend(other)
        , active(other.active.load())
        , health(other.health.load())
        , retry_at_ms(other.retry_at_ms.load())
    {
    }

    group_backend&
    group_backend::operator=(group_backend const& other)
    {
        if (this != &other)
        {
            upstream_backend::operator=(other);
            active.store(other.active.load());
            health.store(other.health.load());
            retry_at_ms.store(other.retry_at_ms.load());
        }
        return *this;
    }

    group_backend::group_backend(group_backend&& other) noexcept
        : upstream_backend(std::move(other))
        , active(other.active.load())
        , health(other.health.load())
        , retry_at_ms(other.retry_at_ms.load())
    {
    }

    group_backend&
    group_backend::operator=(group_backend&& other) noexcept
    {
        if (this != &other)
        {
            upstream_backend::operator=(std::move(other));
            active.store(other.active.load());
            health.store(other.health.load());
            retry_at_ms.store(other.retry_at_ms.load());
        }
        return *this;
    }

    namespace
    {
        std::int64_t
        now_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        bool
        is_healthy(group_backend const& b)
        {
            return b.health.load(std::memory_order_acquire) == backend_health::healthy;
        }
    } // namespace

    upstream_group::upstream_group(std::vector<group_backend> backends,
                                   upstream_locator locator,
                                   std::shared_ptr<spdlog::logger> logger,
                                   std::chrono::milliseconds unhealthy_cooldown)
        : backends_(std::move(backends))
        , locator_(locator)
        , logger_(std::move(logger))
        , unhealthy_cooldown_(unhealthy_cooldown)
    {
        for (size_t i = 0; i < backends_.size(); ++i)
        {
            if (!url_index_.emplace(backends_[i].url, i).second)
            {
                throw std::invalid_argument("upstream_group: duplicate upstream url: " + backends_[i].url);
            }
        }
    }

    void
    upstream_group::on_acquired(std::string const& url)
    {
        auto it = url_index_.find(url);
        if (it == url_index_.end())
        {
            if (logger_)
            {
                logger_->debug("upstream_group: on_acquired miss for {}", url);
            }
            return;
        }
        backends_[it->second].active.fetch_add(1, std::memory_order_relaxed);
    }

    void
    upstream_group::on_released(std::string const& url)
    {
        auto it = url_index_.find(url);
        if (it == url_index_.end())
        {
            if (logger_)
            {
                logger_->debug("upstream_group: on_released miss for {}", url);
            }
            return;
        }
        backends_[it->second].active.fetch_sub(1, std::memory_order_relaxed);
    }

    std::uint64_t
    upstream_group::acquire_ticket(std::string const& url)
    {
        auto it = url_index_.find(url);
        if (it == url_index_.end())
        {
            if (logger_)
            {
                logger_->debug("upstream_group: acquire_ticket miss for {}", url);
            }
            return 0;
        }

        auto& backend = backends_[it->second];
        backend.active.fetch_add(1, std::memory_order_relaxed);
        return static_cast<std::uint64_t>(it->second) + 1;
    }

    void
    upstream_group::release_ticket(std::uint64_t ticket) noexcept
    {
        if (ticket == 0 || ticket > backends_.size())
        {
            if (logger_)
            {
                logger_->debug("upstream_group: release_ticket invalid ticket {}", ticket);
            }
            return;
        }

        auto& backend = backends_[ticket - 1];
        backend.active.fetch_sub(1, std::memory_order_relaxed);

        // A release without a success/failure result means the probe request was
        // cancelled or torn down before it could decide. Re-arm the cooldown instead
        // of leaving the backend stuck in half_open. Store retry_at before flipping
        // the state so a concurrent probe selection cannot observe a stale deadline.
        if (backend.health.load(std::memory_order_acquire) == backend_health::half_open)
        {
            backend.retry_at_ms.store(now_ms() + unhealthy_cooldown_.count(), std::memory_order_release);
            auto expected = backend_health::half_open;
            if (backend.health.compare_exchange_strong(
                    expected, backend_health::unhealthy, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                if (logger_)
                {
                    logger_->debug("upstream_group: probe cancelled for {}", backend.url);
                }
            }
        }
    }

    void
    upstream_group::report_success(std::uint64_t ticket) noexcept
    {
        if (ticket == 0 || ticket > backends_.size())
        {
            if (logger_)
            {
                logger_->debug("upstream_group: report_success invalid ticket {}", ticket);
            }
            return;
        }

        auto& backend = backends_[ticket - 1];
        backend.retry_at_ms.store(0, std::memory_order_release);
        backend.health.store(backend_health::healthy, std::memory_order_release);
    }

    void
    upstream_group::report_failure(std::uint64_t ticket, boost::system::error_code const& ec) noexcept
    {
        if (ticket == 0 || ticket > backends_.size())
        {
            if (logger_)
            {
                logger_->debug("upstream_group: report_failure invalid ticket {}", ticket);
            }
            return;
        }

        auto& backend = backends_[ticket - 1];
        backend.retry_at_ms.store(now_ms() + unhealthy_cooldown_.count(), std::memory_order_release);
        backend.health.store(backend_health::unhealthy, std::memory_order_release);
        if (logger_)
        {
            logger_->debug("upstream_group: backend {} unhealthy: {}", backend.url, ec.message());
        }
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

    group_backend*
    upstream_group::try_acquire_probe()
    {
        auto now = now_ms();
        for (auto& b : backends_)
        {
            if (b.health.load(std::memory_order_acquire) != backend_health::unhealthy)
            {
                continue;
            }
            if (now < b.retry_at_ms.load(std::memory_order_relaxed))
            {
                continue;
            }

            auto expected = backend_health::unhealthy;
            if (b.health.compare_exchange_strong(
                    expected, backend_health::half_open, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                if (logger_)
                {
                    logger_->debug("upstream_group: probing {}", b.url);
                }
                return &b;
            }
        }
        return nullptr;
    }

    group_backend&
    upstream_group::do_resolve()
    {
        // Passive half-open recovery: let at most one real request probe a due
        // backend, even when other healthy backends are available. Without this,
        // an unhealthy backend with no traffic would never recover.
        if (auto* probe = try_acquire_probe())
        {
            return *probe;
        }

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

    net::awaitable<std::string>
    upstream_group::url(request&)
    {
        co_return resolve_url();
    }

    std::string const&
    upstream_group::resolve_url()
    {
        auto& b = do_resolve();
        return b.url;
    }

    std::vector<group_backend*>
    upstream_group::healthy_backends()
    {
        std::vector<group_backend*> result;
        for (auto& b : backends_)
        {
            if (is_healthy(b))
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
            backends_[idx].retry_at_ms.store(now_ms() + unhealthy_cooldown_.count(), std::memory_order_release);
            backends_[idx].health.store(backend_health::unhealthy, std::memory_order_release);
        }
    }

    void
    upstream_group::mark_healthy(size_t idx)
    {
        if (idx < backends_.size())
        {
            backends_[idx].retry_at_ms.store(0, std::memory_order_release);
            backends_[idx].health.store(backend_health::healthy, std::memory_order_release);
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
            if (is_healthy(backends_[idx]))
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
            bool healthy = is_healthy(backends_[i]);
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
            if (!is_healthy(b))
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
