#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace httplib::server::middleware
{

    /// 桶表达到容量上限时如何处理未见过的 IP。
    enum class capacity_action
    {
        /// 淘汰最久未访问的桶，为新客户端腾位（默认）。每个请求都会被计数，限流
        /// 始终有效，内存有界。代价是 IP 轮换的攻击者可以挤掉别人的桶，等于重置
        /// 了那些客户端的计数——但这与「按 IP 限流」本身的语义一致：任何按 IP 的
        /// 限流都无法约束跨 IP 的总量。
        evict_oldest,

        /// 直接返回 429 拒绝。保护性最强，攻击者无法通过轮换 IP 获得额外配额；
        /// 代价是攻击者只要占满桶表，就能把之后到达的新客户端全部挡在门外。
        reject,
    };

    class HTTPLIB_API rate_limit_middleware
    {
      public:
        rate_limit_middleware(uint32_t max_requests, std::chrono::steady_clock::duration window);
        ~rate_limit_middleware();

        /// 最多同时跟踪多少个客户端（按 IP），默认 8192。
        ///
        /// 达到上限后的行为由 when_full() 决定；无论哪种策略，内存占用都在
        /// 此封顶。桶满时先顺带回收空闲桶，因此正常流量下不会触发该分支。
        rate_limit_middleware& max_tracked_clients(std::size_t n);

        /// 桶的空闲保留时长，默认与 window 相同。超过该时长未被访问的桶会在后续
        /// 请求中被顺带回收（回收扫描按 window 节流，不另起线程）。设为 0 表示
        /// 不做时间回收，仅保留上面的容量上限。
        rate_limit_middleware& idle_expiration(std::chrono::steady_clock::duration d);

        /// 桶满时对未见过的 IP 的处理策略，默认 evict_oldest。
        rate_limit_middleware& when_full(capacity_action action);

        /// 当前被跟踪的桶数（主要用于测试与可观测性）。
        std::size_t tracked_clients() const;

        bool before(request& req, response& resp);

      private:
        class impl;
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
