#pragma once

#include "httplib/db/connection_pool.hpp"
#include "httplib/db/session.hpp"
#include "httplib/util/ticker.hpp"
#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace httplib::db
{

    struct connection_pool::impl : public util::ticker
    {
        impl(net::any_io_executor ex, pool_params cfg, connection_pool::connect_fn connect);
        ~impl();

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> l);

        net::awaitable<session_handle> async_acquire(std::chrono::steady_clock::duration wait_timeout);
        void release_session(std::unique_ptr<session> sess, uint64_t epoch);
        void stop() override;

        size_t active_count() const;
        size_t idle_count() const;
        size_t total_count() const;

        net::any_io_executor
        get_executor() const noexcept
        {
            return ex_;
        }

      protected:
        net::awaitable<bool> on_start() override;
        net::awaitable<bool> on_tick() override;

      private:
        using waiters_list = std::deque<std::weak_ptr<net::steady_timer>>;

        std::unique_ptr<session> try_pop_idle(std::vector<std::unique_ptr<session>>& discarded);
        /// 取出一条可借出的空闲连接；第二项为取用时刻的池轮次（与 inc_active 同临界区取样），
        /// ping 校验后据此判断该连接是否已因 stop/重启过期。
        net::awaitable<std::pair<std::unique_ptr<session>, uint64_t>> try_pop_validated();
        void wake_one_waiter();
        void push_idle(std::unique_ptr<session> sess, uint64_t epoch);
        net::awaitable<std::unique_ptr<session>> create_session();

        mutable std::mutex mutex_;
        std::vector<std::unique_ptr<session>> idle_;
        size_t active_count_ = 0;
        /// 维护协程正在健康检查（ping 中）的连接数：不在 idle_ 里，但必须占容量，
        /// 否则借出侧会在验证窗口内误判有空位而超建连接（且 total_count 少报）。
        size_t validating_ = 0;
        waiters_list waiters_;

        /// 每轮生命周期标识：stop() 递增。维护协程在 on_start/on_tick 入口取样，
        /// 在 await 之后比对，丢弃 stop/重启后旧轮的过期副作用。
        std::atomic<uint64_t> epoch_ { 0 };

        net::any_io_executor ex_;
        pool_params cfg_;
        connection_pool::connect_fn connect_;

        std::shared_ptr<spdlog::logger> default_logger_;
        std::atomic<std::shared_ptr<spdlog::logger>> custom_logger_;

        // ---- 持锁计数辅助（调用前必须已持有 mutex_）----

        /// 池中物理连接总数 = 借出中 + 空闲 + 验证中（ping 中）。
        size_t
        total_locked() const noexcept
        {
            return active_count_ + idle_.size() + validating_;
        }

        /// 是否还可建新连接（未达 max_connections）。
        bool
        has_capacity_locked() const noexcept
        {
            return total_locked() < cfg_.max_connections;
        }

        void
        inc_active_locked() noexcept
        {
            ++active_count_;
        }

        /// 递减借出计数；带下溢防御（理论上每次递减对应一次递增）。
        void
        dec_active_locked() noexcept
        {
            if (active_count_ > 0)
            {
                --active_count_;
            }
        }
    };

} // namespace httplib::db
