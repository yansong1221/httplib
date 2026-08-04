#include "httplib/server/middleware/rate_limit.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <mutex>
#include <unordered_map>

namespace httplib::server::middleware
{

    class rate_limit::impl
    {
      public:
        using clock = std::chrono::steady_clock;
        using time_point = clock::time_point;
        using duration = clock::duration;

        struct bucket
        {
            uint32_t count = 0;
            time_point window_start;
        };

        uint32_t max_requests;
        duration window;
        std::mutex mutex;
        std::unordered_map<std::string, bucket> buckets;
    };

    rate_limit::rate_limit(uint32_t max_requests, std::chrono::steady_clock::duration window)
        : impl_(new impl { max_requests, window, {}, {} })
    {
    }

    rate_limit::~rate_limit() = default;

    bool
    rate_limit::before(request& req, response& resp)
    {
        auto ip = req.get_client_ip().to_string();
        auto now = std::chrono::steady_clock::now();

        std::lock_guard lock(impl_->mutex);
        auto& b = impl_->buckets[ip];

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
                http::status::too_many_requests);
            return false;
        }

        ++b.count;
        return true;
    }

} // namespace httplib::server::middleware
