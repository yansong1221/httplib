#pragma once
#include "httplib/config.hpp"
#include "httplib/util/action_queue.hpp"
#include "httplib/util/async_event.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

namespace httplib::util
{
    /// Serialises fire-and-forget coroutine handlers on a single executor.
    ///
    /// Exactly one worker coroutine is spawned when the queue is created and
    /// stays parked on `wake_event_` until there is work or a stop request, so
    /// handlers never overlap and `push()` never has to start machinery
    /// itself. `push()` is safe from any thread: producers enqueue under
    /// `mutex_` and then wake the worker through a latched `async_event`, so a
    /// push racing with the worker parking is never lost.
    ///
    /// State (all guarded by `mutex_`):
    ///
    ///     shutting_down_    no new work is accepted; the worker drains and exits
    ///                       (graceful) or is aborted (cancel)
    ///
    /// `cancel()` clears the backlog, while a graceful shutdown keeps it, so the
    /// worker needs no separate "cancelled" flag: after the running handler it
    /// simply returns to the loop head, where an empty queue plus
    /// `shutting_down_` means stop while a non-empty queue means keep draining.
    ///
    /// On the terminal path `wake_event_` is closed (sticky wake of the parked
    /// worker) and the worker signals `stop_event_` by closing it, which lets
    /// any number of shutdown callers observe completion without a re-check
    /// loop: `close()` latches for waiters that arrive later, unlike
    /// `notify_all()`.
    class action_queue::impl : public std::enable_shared_from_this<action_queue::impl>
    {
      public:
        /// Builds the queue and immediately spawns its worker. The worker is
        /// started after `this` is owned by a `shared_ptr`, so it may keep the
        /// implementation alive across its suspensions.
        static std::shared_ptr<impl>
        create(net::any_io_executor const& executor, std::size_t max_pending, action_queue::error_handler_t on_error)
        {
            // `new` (not make_shared) so this member can reach the private ctor.
            auto self = std::shared_ptr<impl>(new impl(executor, max_pending, std::move(on_error)));
            self->spawn_worker();
            return self;
        }

        impl(impl const&) = delete;
        impl& operator=(impl const&) = delete;

        boost::system::error_code
        push(act_t handler)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (shutting_down_)
                {
                    return boost::system::errc::make_error_code(boost::system::errc::operation_canceled);
                }

                if (queue_.size() >= max_pending_)
                {
                    return boost::system::errc::make_error_code(boost::system::errc::resource_unavailable_try_again);
                }

                queue_.push(std::move(handler));
            }

            wake_event_.notify_one();
            return {};
        }

        void
        clear()
        {
            std::queue<act_t> dropped;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                std::swap(queue_, dropped);
            }
        }

        std::size_t
        pending() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.size();
        }

        void
        cancel()
        {
            std::shared_ptr<net::cancellation_signal> signal;
            {
                std::queue<act_t> dropped;
                std::lock_guard<std::mutex> lock(mutex_);
                shutting_down_ = true;
                signal = current_signal_;
                std::swap(queue_, dropped);
            }

            // Abort the running handler (if any) and wake the parked worker so
            // it observes the stop request.
            if (signal)
            {
                signal->emit(net::cancellation_type::all);
            }
            wake_event_.close();
        }

        net::any_io_executor
        get_executor() const
        {
            return executor_;
        }

        net::awaitable<void>
        async_shutdown()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                shutting_down_ = true;
            }

            // Graceful shutdown: the worker drains the remaining handlers and
            // then exits. `stop_event_` is closed by the worker, so this wait
            // returns immediately for the first caller and for every later one.
            wake_event_.close();
            (void)co_await stop_event_.wait();
        }

      private:
        impl(net::any_io_executor const& executor, std::size_t max_pending, action_queue::error_handler_t on_error)
            : executor_(executor)
            , max_pending_(max_pending)
            , on_error_(std::move(on_error))
            , wake_event_(executor)
            , stop_event_(executor)
        {
        }

        void
        spawn_worker()
        {
            net::co_spawn(
                executor_,
                [self = shared_from_this()]() -> net::awaitable<void>
                {
                    try
                    {
                        co_await self->run();
                    }
                    catch (...)
                    {
                        // run() is not supposed to throw; keep shutdown waiters
                        // released and retire the queue.
                        self->report_error(std::current_exception());
                        self->fail_worker();
                    }
                },
                net::detached);
        }

        net::awaitable<void>
        run()
        {
            auto self = shared_from_this(); // keeps impl alive while suspended

            for (;;)
            {
                act_t handler;
                std::shared_ptr<net::cancellation_signal> signal;

                {
                    std::unique_lock<std::mutex> lock(mutex_);

                    if (queue_.empty())
                    {
                        if (shutting_down_)
                        {
                            lock.unlock();
                            stop_event_.close();
                            co_return;
                        }

                        lock.unlock();
                        // Woken by a push, or closed by a stop request; either
                        // way the loop re-checks the queue and state above.
                        (void)co_await wake_event_.wait();
                        continue;
                    }

                    handler = std::move(queue_.front());
                    queue_.pop();

                    signal = std::make_shared<net::cancellation_signal>();
                    current_signal_ = signal;
                }

                // `signal` outlives the handler's coroutine frame, whose
                // cancellation slot points into it.
                try
                {
                    co_await net::co_spawn(executor_,
                                           std::move(handler),
                                           net::bind_cancellation_slot(signal->slot(), net::use_awaitable));
                }
                catch (boost::system::system_error const& e)
                {
                    // operation_aborted is expected when cancel() fires; any
                    // other failure is reported but the worker keeps draining.
                    if (e.code() != net::error::operation_aborted)
                    {
                        report_error(std::current_exception());
                    }
                }
                catch (...)
                {
                    report_error(std::current_exception());
                }

                std::unique_lock<std::mutex> lock(mutex_);
                current_signal_.reset();
            }
        }

        /// Terminal recovery for an unexpected worker failure.
        void
        fail_worker()
        {
            std::queue<act_t> dropped;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                shutting_down_ = true;
                current_signal_.reset();
                std::swap(queue_, dropped);
            }
            wake_event_.close();
            stop_event_.close();
        }

        void
        report_error(std::exception_ptr ep) noexcept
        {
            if (!on_error_)
            {
                return;
            }
            try
            {
                on_error_(ep);
            }
            catch (...)
            {
            }
        }

        net::any_io_executor executor_;
        std::size_t max_pending_;
        action_queue::error_handler_t on_error_;

        mutable std::mutex mutex_;
        std::queue<act_t> queue_;
        std::shared_ptr<net::cancellation_signal> current_signal_;

        // Guarded by mutex_.
        bool shutting_down_ = false;

        async_event wake_event_;
        async_event stop_event_;
    };
} // namespace httplib::util
