#pragma once
#include "httplib/server/middleware/session.hpp"
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace httplib::server::middleware
{

    class HTTPLIB_API memory_session_store : public session_store
    {
      public:
        explicit memory_session_store(std::chrono::seconds ttl = std::chrono::hours(24));
        ~memory_session_store() override;

        std::shared_ptr<session> load(std::string_view id) override;
        void save(session const& s) override;
        void destroy(std::string_view id) override;
        void set_max_sessions(std::size_t max_sessions) override;

        /// 当前保留的会话数（不含已过期但尚未被回收的条目）。
        std::size_t size() const;

        /// 立即回收全部已过期会话。正常路径下 `save()` 会按 TTL 节流顺带回收，
        /// 该方法供需要立刻释放内存的调用方使用。
        void cleanup();

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
