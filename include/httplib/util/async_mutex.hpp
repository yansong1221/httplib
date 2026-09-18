#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <memory>

namespace httplib::util
{

    /**
     * @brief 异步互斥锁的结果状态。
     */
    enum class lock_status : std::uint8_t
    {
        acquired,    ///< 成功获得锁
        would_block, ///< try_lock() 时锁已经被占用
        timeout,     ///< 等待超时
        cancelled,   ///< 等待期间协程被取消
        closed,      ///< mutex 已经关闭
        failed       ///< 其它内部错误
    };

    /**
     * @brief 跨 co_await 的异步互斥锁。
     *
     * 设计目标：
     *
     *  1. 锁可以跨越任意数量的 co_await。
     *  2. 同一时刻最多一个 owner。
     *  3. waiter FIFO。
     *  4. unlock 时直接把 ownership 转交给队首 waiter，
     *     避免 unlock -> unlocked -> waiter 再竞争的空窗。
     *  5. 等待期间 cancellation / timeout 不会错误消耗锁。
     *  6. close() 可以唤醒全部 waiter。
     *  7. close() 后禁止新的 lock。
     *  8. 已经持有锁的 owner 在 close() 后仍然可以正常退出。
     *  9. guard 为 move-only RAII 对象。
     * 10. token 对业务层隐藏。
     * 11. async_mutex 内部状态由 shared_ptr<impl> 管理，
     *     waiter / guard 不依赖 async_mutex 对象本身继续存在。
     *
     * 生命周期：
     *
     *     async_mutex
     *          │
     *          └── shared_ptr<impl>
     *                    │
     *             ┌──────┴──────┐
     *             │             │
     *          waiter         guard
     *
     * async_mutex 析构时：
     *
     *     close()
     *        │
     *        ├── waiter -> closed
     *        │
     *        └── owner -> 仍然有效
     *
     * guard 自己持有 impl，因此 mutex 对象析构后，
     * guard 仍然可以安全地释放内部 ownership。
     *
     * 注意：
     *
     * async_mutex 对象本身的析构不能与其成员函数调用发生未同步的
     * C++ 对象生命周期竞争。例如：
     *
     *     Thread A: mutex.lock()
     *     Thread B: delete mutex
     *
     * 这种场景需要由外部生命周期管理保证安全。
     */
    class HTTPLIB_API async_mutex
    {
      public:
        class HTTPLIB_API guard;

      private:
        class impl;

      public:
        using duration = std::chrono::steady_clock::duration;

        explicit async_mutex(net::any_io_executor ex);

        ~async_mutex();

        async_mutex(async_mutex const&) = delete;
        async_mutex& operator=(async_mutex const&) = delete;

        async_mutex(async_mutex&&) = delete;
        async_mutex& operator=(async_mutex&&) = delete;

        /**
         * @brief 返回 mutex 使用的 executor。
         */
        [[nodiscard]]
        net::any_io_executor get_executor() const noexcept;

        /**
         * @brief 异步获取锁。
         *
         * 无限等待，直到：
         *
         *     acquired
         *     cancelled
         *     closed
         *     failed
         *
         * 使用：
         *
         *     auto guard = co_await mutex.lock();
         *
         *     if (!guard)
         *         co_return;
         *
         *     co_await write_header();
         *     co_await write_body();
         */
        [[nodiscard]]
        net::awaitable<guard> lock();

        /**
         * @brief 限时获取锁。
         *
         * timeout 到期时返回 timeout。
         *
         * timeout <= 0 时不会等待。
         */
        [[nodiscard]]
        net::awaitable<guard> lock_for(duration timeout);

        /**
         * @brief 非阻塞尝试获取锁。
         *
         * 成功：
         *
         *     guard.status() == lock_status::acquired
         *
         * 锁已经被其它 owner 持有：
         *
         *     guard.status() == lock_status::would_block
         */
        [[nodiscard]]
        guard try_lock();

        /**
         * @brief 当前是否有 owner。
         */
        [[nodiscard]]
        bool is_locked() const noexcept;

        /**
         * @brief mutex 是否已经关闭。
         */
        [[nodiscard]]
        bool is_closed() const noexcept;

        /**
         * @brief 当前等待者数量。
         *
         * 该值主要用于诊断 / metrics。
         *
         * 注意：
         * 这是瞬时值，不应该用于同步逻辑。
         */
        [[nodiscard]]
        std::size_t waiter_count() const noexcept;

        /**
         * @brief 关闭 mutex。
         *
         * close() 后：
         *
         *     新 lock       -> closed
         *     新 try_lock   -> closed
         *     新 lock_for   -> closed
         *
         * 已经等待的 waiter：
         *
         *     -> closed
         *
         * 已经持有锁的 owner：
         *
         *     -> 不受影响
         *     -> 仍然可以正常释放 guard
         *
         * close() 幂等。
         */
        void close() noexcept;

      private:
        friend class guard;

        std::shared_ptr<impl> impl_;
    };

    /**
     * @brief async_mutex 的 RAII ownership guard。
     *
     * guard 有两种状态：
     *
     *     acquired
     *         当前 guard 真正持有 mutex。
     *
     *     其它状态
     *         当前 guard 不持有 mutex。
     *
     * 因此：
     *
     *     if (guard)
     *     {
     *         // 当前持锁
     *     }
     *
     * guard 为 move-only。
     *
     * move 后：
     *
     *     source.owns_lock() == false
     *
     * 从而避免 double unlock。
     */
    class async_mutex::guard
    {
      public:
        guard() noexcept;

        ~guard() noexcept;

        guard(guard const&) = delete;
        guard& operator=(guard const&) = delete;

        guard(guard&& other) noexcept;

        guard& operator=(guard&& other) noexcept;

        /**
         * @brief 当前是否真正持有锁。
         */
        [[nodiscard]]
        bool owns_lock() const noexcept;

        /**
         * @brief 与 owns_lock() 等价。
         */
        [[nodiscard]]
        explicit operator bool() const noexcept;

        /**
         * @brief 获取 lock 状态。
         */
        [[nodiscard]]
        lock_status status() const noexcept;

        /**
         * @brief 主动释放锁。
         *
         * 调用之后：
         *
         *     owns_lock() == false
         *
         * reset() 幂等。
         */
        void reset() noexcept;

        /**
         * @brief reset() 的语义别名。
         */
        void unlock() noexcept;

      private:
        friend class async_mutex;
        friend class async_mutex::impl;

        /**
         * @brief 成功 ownership 的 guard。
         */
        guard(std::shared_ptr<impl> impl, std::uint64_t token) noexcept;

        /**
         * @brief 非 owner 状态 guard。
         *
         * 用于：
         *
         *     would_block
         *     timeout
         *     cancelled
         *     closed
         *     failed
         */
        explicit guard(lock_status status) noexcept;

      private:
        std::shared_ptr<impl> impl_;
        std::uint64_t token_ = 0;
        lock_status status_ = lock_status::failed;
        bool active_ = false;
    };

} // namespace httplib::util
