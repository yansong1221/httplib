#include "httplib/server/middleware/rate_limit.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/util/string_hash.hpp"
#include <mutex>
#include <unordered_map>
#include "beast_alias.hpp"

namespace httplib::server::middleware
{

    class rate_limit_middleware::impl
    {
      public:
        using clock = std::chrono::steady_clock;
        using time_point = clock::time_point;
        using duration = clock::duration;

        struct bucket
        {
            uint32_t count = 0;
            time_point window_start;
            time_point last_seen;
        };

        uint32_t max_requests;
        duration window;
        std::mutex mutex;
        util::string_map<bucket> buckets;

        std::size_t max_tracked = 8192;
        duration idle_expiration;
        time_point last_sweep {};
        capacity_action action = capacity_action::evict_oldest;

        /// 顺带回收空闲桶：按 idle_expiration 节流，因此单次请求的额外成本是
        /// 均摊的 O(桶数)，不需要后台线程，也不会在高频请求下反复全表扫描。
        void
        sweep(time_point now)
        {
            if (idle_expiration <= duration::zero())
            {
                return;
            }
            if (last_sweep != time_point {} && now - last_sweep < idle_expiration)
            {
                return;
            }
            last_sweep = now;

            for (auto it = buckets.begin(); it != buckets.end();)
            {
                if (now - it->second.last_seen > idle_expiration)
                {
                    it = buckets.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        /// 淘汰最久未访问的桶给新客户端腾位。只在桶满且 sweep 已回收过之后调用，
        /// 因此 O(桶数) 的扫描不会出现在高频路径上。
        void
        evict_oldest()
        {
            auto oldest = buckets.begin();
            for (auto it = buckets.begin(); it != buckets.end(); ++it)
            {
                if (it->second.last_seen < oldest->second.last_seen)
                {
                    oldest = it;
                }
            }
            buckets.erase(oldest);
        }
    };

    rate_limit_middleware::rate_limit_middleware(uint32_t max_requests, std::chrono::steady_clock::duration window)
        : impl_(new impl { max_requests, window, {}, {}, 8192, window, {}, capacity_action::evict_oldest })
    {
    }

    rate_limit_middleware::~rate_limit_middleware() = default;

    rate_limit_middleware&
    rate_limit_middleware::max_tracked_clients(std::size_t n)
    {
        impl_->max_tracked = n;
        return *this;
    }

    rate_limit_middleware&
    rate_limit_middleware::idle_expiration(std::chrono::steady_clock::duration d)
    {
        impl_->idle_expiration = d;
        return *this;
    }

    rate_limit_middleware&
    rate_limit_middleware::when_full(capacity_action action)
    {
        impl_->action = action;
        return *this;
    }

    std::size_t
    rate_limit_middleware::tracked_clients() const
    {
        std::lock_guard lock(impl_->mutex);
        return impl_->buckets.size();
    }

    bool
    rate_limit_middleware::before(request& req, response& resp)
    {
        auto ip = req.get_client_ip().to_string();
        auto now = std::chrono::steady_clock::now();

        std::lock_guard lock(impl_->mutex);
        impl_->sweep(now);

        auto iter = impl_->buckets.find(ip);
        if (iter == impl_->buckets.end())
        {
            if (impl_->buckets.size() >= impl_->max_tracked)
            {
                if (impl_->action == capacity_action::reject)
                {
                    auto retry_after = std::chrono::duration_cast<std::chrono::seconds>(impl_->window).count();
                    resp.set(std::string_view("Retry-After"), std::to_string(retry_after));
                    resp.set_json_content(
                        {
                            { "error", "rate limit client table is full" }
                    },
                        status::too_many_requests);
                    return false;
                }
                // 腾位而不是放行：否则 IP 轮换的攻击者能完全绕过限流，
                // 限流在最需要它的时刻（桶表被占满）反而失效。
                impl_->evict_oldest();
            }
            iter = impl_->buckets.emplace(ip, impl::bucket { 0, now, now }).first;
        }

        auto& b = iter->second;
        b.last_seen = now;

        if (now - b.window_start > impl_->window)
        {
            b.window_start = now;
            b.count = 0;
        }

        if (b.count >= impl_->max_requests)
        {
            auto retry_after
                = std::chrono::duration_cast<std::chrono::seconds>((b.window_start + impl_->window) - now).count();
            resp.set(std::string_view("Retry-After"), std::to_string(retry_after));
            resp.set_json_content(
                {
                    { "error", "too many requests" }
            },
                status::too_many_requests);
            return false;
        }

        ++b.count;
        return true;
    }

} // namespace httplib::server::middleware
