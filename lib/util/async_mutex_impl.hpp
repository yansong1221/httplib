#pragma once
#include "httplib/util/async_event.hpp"
#include "httplib/util/async_mutex.hpp"
#include <boost/asio/this_coro.hpp>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace httplib::util
{

    /**
     * @brief async_mutex 内部实现。
     *
     * async_mutex 本身只是：
     *
     *     shared_ptr<impl>
     *
     * 所有异步状态都放在 impl 中。
     */
    class async_mutex::impl : public std::enable_shared_from_this<async_mutex::impl>
    {
      public:
        explicit impl(net::any_io_executor ex) : executor_(std::move(ex)) {}

        ~impl() = default;

        struct waiter
        {
            std::shared_ptr<async_event> event;

            /*
             * unlock() 直接 ownership handoff 时生成。
             */
            std::uint64_t token = 0;

            /*
             * true：
             *
             *     waiter 仍然在 waiters_ 中。
             *
             * false：
             *
             *     ownership 已经直接交给这个 waiter。
             *
             * 注意：
             *
             * queued == false 并不表示 waiter 已经 resume。
             * 它只表示 state machine 已经把 ownership 交给它。
             */
            bool queued = true;
        };

        [[nodiscard]]
        net::any_io_executor
        get_executor() const noexcept
        {
            return executor_;
        }

        [[nodiscard]]
        lock_status
        status_for_try_lock() const noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_)
            {
                return lock_status::closed;
            }

            if (locked_)
            {
                return lock_status::would_block;
            }

            return lock_status::acquired;
        }

        [[nodiscard]]
        async_mutex::guard
        try_lock()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_)
            {
                return async_mutex::guard(lock_status::closed);
            }

            if (locked_)
            {
                return async_mutex::guard(lock_status::would_block);
            }

            return acquire_locked();
        }

        [[nodiscard]]
        net::awaitable<async_mutex::guard>
        async_lock()
        {
            auto executor = co_await net::this_coro::executor;

            std::shared_ptr<waiter> w;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (closed_)
                {
                    co_return async_mutex::guard(lock_status::closed);
                }

                if (!locked_)
                {
                    co_return acquire_locked();
                }

                w = enqueue_locked(executor);
            }

            /*
             * async_event 自己负责：
             *
             *     cancellation
             *     notification
             *
             * 我们这里只负责最终状态机裁决。
             */
            auto const result = co_await w->event->wait();

            co_return finish_wait(w, result);
        }

        [[nodiscard]]
        net::awaitable<async_mutex::guard>
        async_lock_for(duration timeout)
        {
            auto executor = co_await net::this_coro::executor;

            /*
             * 避免负 timeout 进入 event 后产生依赖实现的行为。
             */
            if (timeout <= duration::zero())
            {
                co_return try_lock();
            }

            std::shared_ptr<waiter> w;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (closed_)
                {
                    co_return async_mutex::guard(lock_status::closed);
                }

                if (!locked_)
                {
                    co_return acquire_locked();
                }

                w = enqueue_locked(executor);
            }

            auto const result = co_await w->event->wait_for(timeout);

            co_return finish_wait(w, result);
        }

        void
        unlock(std::uint64_t token) noexcept
        {
            std::shared_ptr<waiter> next;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                /*
                 * ownership invariant：
                 *
                 *     locked_ == true
                 *     owner_ == token
                 */
                if (!locked_ || owner_ == 0 || owner_ != token)
                {
                    /*
                     * 这是内部状态机错误：
                     *
                     *     double unlock
                     *     wrong owner
                     *     invalid token
                     *
                     * release 模式静默忽略，
                     * debug 模式直接暴露错误。
                     */
                    assert(false && "async_mutex: unlock without ownership");

                    return;
                }

                /*
                 * 先清除当前 owner。
                 */
                locked_ = false;
                owner_ = 0;

                /*
                 * close() 后不再把 ownership 交给 waiter。
                 */
                if (closed_)
                {
                    return;
                }

                /*
                 * 没有 waiter。
                 */
                if (waiters_.empty())
                {
                    return;
                }

                /*
                 * FIFO ownership handoff。
                 *
                 * 注意：
                 *
                 * 这里不能先：
                 *
                 *     locked_ = false
                 *
                 * 然后 notify waiter，
                 * 再让 waiter 重新竞争。
                 *
                 * 否则 try_lock() 可以插队。
                 *
                 * 正确方式是：
                 *
                 *     current owner
                 *          ↓
                 *     queue front
                 *
                 * ownership 连续转移。
                 */
                next = std::move(waiters_.front());

                waiters_.pop_front();

                next->queued = false;

                owner_ = next_token_locked();
                next->token = owner_;

                locked_ = true;
            }

            /*
             * notify 必须放在 mutex_ 外面。
             *
             * event->notify_one() 可能触发 executor 调度，
             * 不应该持有内部 mutex。
             */
            next->event->notify_one();
        }

        [[nodiscard]]
        bool
        is_locked() const noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return locked_;
        }

        [[nodiscard]]
        bool
        is_closed() const noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return closed_;
        }

        [[nodiscard]]
        std::size_t
        waiter_count() const noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return waiters_.size();
        }

        void
        close() noexcept
        {
            std::deque<std::shared_ptr<waiter>> waiters;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (closed_)
                {
                    return;
                }

                closed_ = true;

                /*
                 * 已经持有锁的 owner 不受影响。
                 *
                 * 它仍然可以：
                 *
                 *     guard.reset()
                 *
                 * 最终 unlock。
                 *
                 * 但是 unlock 时不会继续 handoff。
                 */
                waiters.swap(waiters_);
            }

            /*
             * 不持有 mutex_ 通知 waiter。
             */
            for (auto& w : waiters)
            {
                w->event->close();
            }
        }

      private:
        [[nodiscard]]
        std::uint64_t
        next_token_locked() noexcept
        {
            /*
             * 0 保留给 invalid token。
             *
             * uint64_t 溢出属于极端情况，
             * 但必须保证永远不会产生 0。
             */
            auto token = ++seq_;

            if (token == 0)
            {
                token = ++seq_;

                /*
                 * 理论上只有整个 uint64_t token 空间都被耗尽
                 * 才会触发。
                 */
                assert(token != 0);
            }

            return token;
        }

        [[nodiscard]]
        async_mutex::guard
        acquire_locked()
        {
            assert(!closed_);
            assert(!locked_);
            assert(owner_ == 0);

            locked_ = true;

            owner_ = next_token_locked();

            assert(owner_ != 0);

            return async_mutex::guard(shared_from_this(), owner_);
        }

        [[nodiscard]]
        std::shared_ptr<waiter>
        enqueue_locked(net::any_io_executor const& executor)
        {
            assert(locked_);
            assert(!closed_);

            auto w = std::make_shared<waiter>();

            w->event = std::make_shared<async_event>(executor);

            waiters_.push_back(w);

            return w;
        }

        void
        remove_locked(std::shared_ptr<waiter> const& w)
        {
            if (!w->queued)
            {
                return;
            }

            for (auto it = waiters_.begin(); it != waiters_.end(); ++it)
            {
                if (it->get() == w.get())
                {
                    waiters_.erase(it);
                    break;
                }
            }

            w->queued = false;
        }

        [[nodiscard]]
        async_mutex::guard
        finish_wait(std::shared_ptr<waiter> const& w, async_event::wait_result result)
        {
            std::lock_guard<std::mutex> lock(mutex_);

            /*
             * 关键竞态：
             *
             * unlock()：
             *
             *     queued = false
             *     token = new token
             *     locked = true
             *     notify()
             *
             * cancellation：
             *
             *     event wait 返回 cancelled
             *
             * 两者可能同时发生。
             *
             * 所以不能单纯相信 result。
             *
             * state machine 才是真正的裁决者。
             */
            if (!w->queued)
            {
                /*
                 * ownership 已经被 unlock() 直接交给这个 waiter。
                 *
                 * 即使 event 返回：
                 *
                 *     cancelled
                 *     timed_out
                 *
                 * 也不能再取消 ownership。
                 *
                 * 否则会造成：
                 *
                 *     mutex 永久 locked
                 *     或 token 泄漏
                 */
                assert(w->token != 0);
                assert(locked_);
                assert(owner_ == w->token);

                return async_mutex::guard(shared_from_this(), w->token);
            }

            /*
             * waiter 仍然在队列中。
             *
             * 说明 ownership 尚未转交。
             *
             * 此时 cancellation / timeout / close
             * 可以安全地取消这个 waiter。
             */
            remove_locked(w);

            switch (result)
            {
                case async_event::wait_result::timed_out:
                    return async_mutex::guard(lock_status::timeout);

                case async_event::wait_result::closed:
                    return async_mutex::guard(lock_status::closed);

                case async_event::wait_result::cancelled:
                    return async_mutex::guard(lock_status::cancelled);

                default:
                    return async_mutex::guard(lock_status::failed);
            }
        }

      private:
        net::any_io_executor executor_;

        mutable std::mutex mutex_;

        /*
         * 状态机：
         *
         *     locked_ == false
         *         owner_ == 0
         *
         *     locked_ == true
         *         owner_ != 0
         */
        bool locked_ = false;

        bool closed_ = false;

        std::uint64_t owner_ = 0;

        std::uint64_t seq_ = 0;

        std::deque<std::shared_ptr<waiter>> waiters_;
    };

} // namespace httplib::util
