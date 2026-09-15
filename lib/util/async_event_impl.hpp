#pragma once
#include "httplib/util/async_event.hpp"
#include <atomic>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/channel_error.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace httplib::util
{

    class async_event::impl
    {
      public:
        using channel_type = net::experimental::concurrent_channel<void(boost::system::error_code)>;

        /**
         * @brief One suspended wait() operation.
         *
         * Each waiter owns a private capacity-1 channel used purely as a
         * one-shot wakeup latch (waking simply closes it). `result` is the
         * single source of truth for *why* the waiter was resumed, guarded by
         * `pending`:
         *
         *     pending == true   -> still queued in waiters_; the event did not
         *                          complete it, so it was resumed by
         *                          cancellation and must remove itself
         *     pending == false  -> the event completed it (notify/close/abort)
         *                          and `result` holds the outcome
         */
        struct waiter
        {
            explicit waiter(net::any_io_executor ex) : channel(std::move(ex), 1) {}

            waiter(waiter const&) = delete;
            waiter& operator=(waiter const&) = delete;

            channel_type channel;
            async_event::wait_result result = async_event::wait_result::failed;
            bool pending = false;
        };

        explicit impl(net::any_io_executor ex) : executor_(std::move(ex)) {}

        impl(impl const&) = delete;
        impl& operator=(impl const&) = delete;

        net::any_io_executor
        get_executor() const
        {
            return executor_;
        }

        async_event::notify_result
        notify_one()
        {
            std::shared_ptr<waiter> target;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (closed_.load(std::memory_order_acquire))
                {
                    return async_event::notify_result::closed;
                }

                if (waiters_.empty())
                {
                    if (signaled_)
                    {
                        return async_event::notify_result::coalesced;
                    }

                    signaled_ = true;
                    return async_event::notify_result::notified;
                }

                target = std::move(waiters_.front());
                waiters_.pop_front();
                target->pending = false;
                target->result = async_event::wait_result::notified;
            }

            // Closing the one-shot channel is an unconditional wake: a pending
            // async_receive completes immediately, and one that has not been
            // initiated yet completes as soon as it is. No payload is needed.
            target->channel.close();
            return async_event::notify_result::notified;
        }

        async_event::notify_result
        notify_all()
        {
            std::vector<std::shared_ptr<waiter>> targets;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (closed_.load(std::memory_order_acquire))
                {
                    return async_event::notify_result::closed;
                }

                if (waiters_.empty())
                {
                    if (signaled_)
                    {
                        return async_event::notify_result::coalesced;
                    }

                    signaled_ = true;
                    return async_event::notify_result::notified;
                }

                targets.reserve(waiters_.size());
                while (!waiters_.empty())
                {
                    auto w = std::move(waiters_.front());
                    waiters_.pop_front();
                    w->pending = false;
                    w->result = async_event::wait_result::notified;
                    targets.push_back(std::move(w));
                }
            }

            for (auto& w : targets)
            {
                w->channel.close();
            }

            return async_event::notify_result::notified;
        }

        bool
        try_wait()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_.load(std::memory_order_acquire))
            {
                return false;
            }

            if (signaled_)
            {
                signaled_ = false;
                return true;
            }

            return false;
        }

        void
        close()
        {
            bool expected = false;
            if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                return;
            }

            std::vector<std::shared_ptr<waiter>> targets;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                signaled_ = false;
                targets.reserve(waiters_.size());
                while (!waiters_.empty())
                {
                    auto w = std::move(waiters_.front());
                    waiters_.pop_front();
                    w->pending = false;
                    w->result = async_event::wait_result::closed;
                    targets.push_back(std::move(w));
                }
            }

            for (auto& w : targets)
            {
                w->channel.close();
            }
        }

        bool
        is_closed() const noexcept
        {
            return closed_.load(std::memory_order_acquire);
        }

        bool
        is_signaled() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return signaled_;
        }

        static net::awaitable<async_event::wait_result> do_wait(std::shared_ptr<impl> self);

        static net::awaitable<async_event::wait_result> do_wait_for(std::shared_ptr<impl> self,
                                                                    std::chrono::steady_clock::duration timeout);

      private:
        // Shared implementation: waits until notified/closed, or until the
        // optional absolute `deadline` elapses (then `timed_out`).
        static net::awaitable<async_event::wait_result> do_wait_until(
            std::shared_ptr<impl> self,
            std::optional<std::chrono::steady_clock::time_point> deadline);

        static bool
        is_cancellation_error(boost::system::error_code ec)
        {
            return ec == boost::asio::error::operation_aborted || ec == net::experimental::error::channel_cancelled
                   || ec == net::experimental::error::channel_closed;
        }

        void
        remove_locked(waiter* w)
        {
            for (auto it = waiters_.begin(); it != waiters_.end(); ++it)
            {
                if (it->get() == w)
                {
                    waiters_.erase(it);
                    return;
                }
            }
        }

        void
        abort_waiter(std::shared_ptr<waiter> const& w, async_event::wait_result result)
        {
            bool removed = false;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (w->pending)
                {
                    remove_locked(w.get());
                    w->pending = false;
                    w->result = result;
                    removed = true;
                }
            }

            if (removed)
            {
                // Unblock the suspended async_receive.
                w->channel.close();
            }
        }

        net::any_io_executor executor_;

        mutable std::mutex mutex_;
        std::atomic<bool> closed_ { false };
        bool signaled_ = false;
        std::deque<std::shared_ptr<waiter>> waiters_;
    };

    inline net::awaitable<async_event::wait_result>
    async_event::impl::do_wait(std::shared_ptr<impl> self)
    {
        return do_wait_until(std::move(self), std::nullopt);
    }

    inline net::awaitable<async_event::wait_result>
    async_event::impl::do_wait_for(std::shared_ptr<impl> self, std::chrono::steady_clock::duration timeout)
    {
        return do_wait_until(std::move(self), std::chrono::steady_clock::now() + timeout);
    }

    inline net::awaitable<async_event::wait_result>
    async_event::impl::do_wait_until(std::shared_ptr<impl> self,
                                     std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        auto w = std::make_shared<waiter>(self->executor_);

        {
            std::lock_guard<std::mutex> lock(self->mutex_);

            if (self->closed_.load(std::memory_order_acquire))
            {
                co_return async_event::wait_result::closed;
            }

            if (self->signaled_)
            {
                self->signaled_ = false;
                co_return async_event::wait_result::notified;
            }

            w->pending = true;
            self->waiters_.push_back(w);
        }

        // With a deadline, a timer on the event's executor removes the waiter
        // and closes its channel on expiry, resuming the receive below with
        // `result == timed_out`. Without one, we just wait indefinitely.
        std::optional<net::steady_timer> timer;
        if (deadline)
        {
            timer.emplace(self->executor_);
            timer->expires_at(*deadline);
            timer->async_wait(
                [self, w](boost::system::error_code tec)
                {
                    if (!tec)
                    {
                        self->abort_waiter(w, async_event::wait_result::timed_out);
                    }
                });
        }

        boost::system::error_code ec;
        co_await w->channel.async_receive(net::redirect_error(net::use_awaitable, ec));

        if (timer)
        {
            // No-op if the timer already fired; otherwise stop it.
            timer->cancel();
        }

        std::lock_guard<std::mutex> lock(self->mutex_);

        if (w->pending)
        {
            // Still queued: the event did not complete this waiter, so it was
            // resumed by the caller's cancellation.
            self->remove_locked(w.get());
            w->pending = false;

            if (self->closed_.load(std::memory_order_acquire))
            {
                co_return async_event::wait_result::closed;
            }

            if (ec && !is_cancellation_error(ec))
            {
                co_return async_event::wait_result::failed;
            }

            co_return async_event::wait_result::cancelled;
        }

        co_return w->result;
    }

} // namespace httplib::util
