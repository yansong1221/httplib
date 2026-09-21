#pragma once

#include "httplib/db/connection_pool.hpp"
#include "httplib/db/session.hpp"
#include "httplib/util/async_event.hpp"
#include "httplib/util/ticker.hpp"
#include "util/logging.hpp"
#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>
#include <cstddef>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace httplib::db
{

    struct connection_pool::impl
        : public httplib::detail::logger
        , public util::ticker
    {
        impl(net::any_io_executor ex, pool_params cfg, connection_pool::connect_fn connect);
        /// 空析构：停止由 connection_pool::~connection_pool() 通过 stop() 触发；
        /// stop() 内部会捕获 self 投递 teardown，若在 ~impl 里再调 stop() 会循环创建 self。
        ~impl();

        net::awaitable<session_handle> async_acquire(std::chrono::steady_clock::duration wait_timeout);
        void release_session(std::unique_ptr<session> sess);
        void stop() override;

        size_t active_count() const;
        size_t idle_count() const;
        size_t total_count() const;

        net::any_io_executor
        get_executor() const noexcept
        {
            return ticker::get_executor();
        }

      protected:
        net::awaitable<bool> on_start() override;
        net::awaitable<bool> on_tick() override;
        net::awaitable<void> on_stop() override;

      private:
        struct waiter_node
        {
            /// 被池唤醒（notify_one）或到达等待截止时间（wait_for 超时）。被唤醒者
            /// 保留在队首直到借出/超时/取消，保证先到先服务。
            util::async_event event;

            explicit waiter_node(net::any_io_executor const& ex) : event(ex) {}
        };

        using waiters_list = std::deque<std::weak_ptr<waiter_node>>;

        // ---- 仅在池自身 strand 上执行的协程/辅助（原 mutex 语义收敛到 strand）----

        /// 取出一条空闲连接（校验/建连统一入口）；借不到时若仍有容量则新建。
        net::awaitable<std::unique_ptr<session>> acquire_or_create(bool serving);
        net::awaitable<void> async_release(std::unique_ptr<session> sess);
        void wake_one_waiter();
        void push_idle(std::unique_ptr<session> sess);
        bool has_live_waiter() noexcept;
        bool can_serve() const noexcept;
        void remove_waiter(std::shared_ptr<waiter_node> const& node);
        /// 释放全部空闲连接并唤醒等待者；仅在 strand 上执行（stop 与 on_stop 兜底调用，幂等）。
        void teardown();

        std::unique_ptr<session> try_pop_idle();
        net::awaitable<std::unique_ptr<session>> create_session();

        // ---- 计数统一由原子维护：strand 上读写（等价普通整数），其他线程只读 ----

        /// 池中物理连接总数 = 借出中 + 空闲 + 验证中（ping 中）。
        ///
        /// 用于 has_capacity 的容量口径含 inflight_（预建占位），
        /// 它占容量但不计入对外总数（见 total_count 的语义差）。
        size_t
        total_size() const noexcept
        {
            return active_metric_.load(std::memory_order_relaxed) + idle_metric_.load(std::memory_order_relaxed)
                   + validating_metric_.load(std::memory_order_relaxed) + inflight_.load(std::memory_order_relaxed);
        }

        /// 是否还可建新连接（未达 max_connections）。
        bool
        has_capacity() const noexcept
        {
            return total_size() < cfg_.max_connections;
        }

        void
        inc_active() noexcept
        {
            ++active_metric_;
        }

        /// 递减借出计数；带下溢防御（理论上每次递减对应一次递增）。
        void
        dec_active() noexcept
        {
            if (active_metric_ > 0)
            {
                --active_metric_;
            }
        }

        // ---- 仅在 strand 上访问的池状态 ----
        std::vector<std::unique_ptr<session>> idle_;
        waiters_list waiters_;
        /// 预建（on_start）尚未落进 idle_ 的连接数：只占容量（参与 has_capacity），
        /// 但既未建立连接也未与客户构成借用，故不计入对外计数 total/active。
        std::atomic<size_t> inflight_ = { 0 };

        // ---- 只在 strand 上更新、可跨线程原子读的计数（单一事实来源）----
        std::atomic<size_t> idle_metric_ { 0 };
        std::atomic<size_t> active_metric_ { 0 };
        /// 维护协程正在健康检查（ping 中）的连接数：不在 idle_ 里，但必须占容量，
        /// 否则借出侧会在验证窗口内误判有空位而超建连接（且 total_count 少报）。
        std::atomic<size_t> validating_metric_ { 0 };

        pool_params cfg_;
        connection_pool::connect_fn connect_;
    };

} // namespace httplib::db
