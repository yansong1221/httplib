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
         * Each waiter owns a private capacity-1 channel. The channel is only
         * ever used as a one-shot wakeup source, so `result` is the single
         * source of truth for *why* the waiter was resumed:
         *
         *     armed == true   -> the waiter was removed by cancellation/abort
         *     armed == false  -> the waiter was completed by notify/close and
         *                        `result` holds the outcome
         */
        struct waiter
        {
            explicit waiter(net::any_io_executor ex) : channel(std::move(ex), 1) {}

            waiter(waiter const&) = delete;
            waiter& operator=(waiter const&) = delete;

            channel_type channel;
            async_event::wait_result result = async_event::wait_result::failed;
            bool armed = false;
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
            for (;;)
            {
                std::shared_ptr<waiter> target;

                {
                    std::lock_guard<std::mutex> lock(mutex_);

                    if (closed_.load(std::memory_order_acquire))
                    {
                        return async_event::notify_result::closed;
                    }

                    if (!waiters_.empty())
                    {
                        target = std::move(waiters_.front());
                        waiters_.pop_front();
                        target->armed = false;
                        target->result = async_event::wait_result::notified;
                    }
                    else
                    {
                        if (signaled_)
                        {
                            return async_event::notify_result::coalesced;
                        }

                        signaled_ = true;
                        return async_event::notify_result::notified;
                    }
                }

                if (target->channel.try_send(boost::system::error_code {}))
                {
                    return async_event::notify_result::notified;
                }

                // The waiter was aborted concurrently; do not lose the
                // notification. Retry with the next waiter or latch it.
            }
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
                    w->armed = false;
                    w->result = async_event::wait_result::notified;
                    targets.push_back(std::move(w));
                }
            }

            for (auto& w : targets)
            {
                w->channel.try_send(boost::system::error_code {});
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
                    w->armed = false;
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
                if (w->armed)
                {
                    remove_locked(w.get());
                    w->armed = false;
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

            w->armed = true;
            self->waiters_.push_back(w);
        }

        boost::system::error_code ec;
        co_await w->channel.async_receive(net::redirect_error(net::use_awaitable, ec));

        std::lock_guard<std::mutex> lock(self->mutex_);

        if (w->armed)
        {
            // The waiter was aborted; the event itself did not complete it.
            self->remove_locked(w.get());
            w->armed = false;

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

    inline net::awaitable<async_event::wait_result>
    async_event::impl::do_wait_for(std::shared_ptr<impl> self, std::chrono::steady_clock::duration timeout)
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

            w->armed = true;
            self->waiters_.push_back(w);
        }

        // The timer's completion handler runs on the event's executor. On
        // expiry it removes the waiter and closes its channel, which resumes
        // the async_receive below with an abort and `result == timed_out`.
        net::steady_timer timer(self->executor_);
        timer.expires_after(timeout);
        timer.async_wait(
            [self, w](boost::system::error_code tec)
            {
                if (!tec)
                {
                    self->abort_waiter(w, async_event::wait_result::timed_out);
                }
            });

        boost::system::error_code ec;
        co_await w->channel.async_receive(net::redirect_error(net::use_awaitable, ec));

        // No-op if the timer already fired; otherwise stop it.
        timer.cancel();

        std::lock_guard<std::mutex> lock(self->mutex_);

        if (w->armed)
        {
            // The event itself did not complete the waiter, so the receive was
            // aborted by the caller's cancellation.
            self->remove_locked(w.get());
            w->armed = false;

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
