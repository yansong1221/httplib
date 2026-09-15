#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <memory>
#include <utility>

namespace httplib::util
{

    /**
     * @brief Thread-safe, coalescing asynchronous event.
     *
     * async_event represents a binary/coalescing notification:
     *
     *     NOT_SIGNALED <----> SIGNALED
     *
     * notify_one():
     *     - changes NOT_SIGNALED -> SIGNALED
     *     - wakes one waiter if present
     *     - repeated notifications are coalesced
     *
     * notify_all():
     *     - wakes every currently waiting coroutine
     *     - coalesces into the latched SIGNALED state when no waiter is present
     *
     * wait():
     *     - consumes the SIGNALED state
     *     - suspends if the event is not signaled
     *     - may return `cancelled` when the awaiting coroutine is cancelled
     *
     * wait_for():
     *     - like wait(), but returns `timed_out` after the given duration
     *
     * try_wait():
     *     - non-blocking consume; returns true iff a latched notification was
     *       consumed
     *
     * close():
     *     - permanently closes the event
     *     - wakes all pending waiters with wait_result::closed
     *     - subsequent notify_one()/notify_all() calls return
     *       notify_result::closed
     *     - subsequent wait() calls return wait_result::closed
     *
     * reset():
     *     - clears any latched notification and reopens a closed event, so the
     *       event can be reused
     *     - intended to be called when no wait() is in flight
     *
     * Important:
     *
     *     This is NOT a counting semaphore.
     *
     *     notify_one();
     *     notify_one();
     *     notify_one();
     *
     *     still represents only one pending notification.
     *
     * It is intended for:
     *
     *     action queue wakeup
     *     cache invalidation
     *     configuration reload
     *     state change notification
     *     worker wakeup
     *     shutdown notification
     *
     * Thread safety:
     *
     *     notify_one(), notify_all(), try_wait(), close(), is_closed() and
     *     is_signaled() may be called from arbitrary threads.
     *
     * Lifetime:
     *
     *     A wait()/wait_for() operation keeps the underlying state alive for
     *     as long as the returned awaitable is running. Destroying the
     *     async_event while a wait is in flight closes the event and wakes the
     *     waiter with wait_result::closed; it will never access freed memory.
     */
    class HTTPLIB_API async_event
    {
      public:
        enum class notify_result
        {
            notified,
            coalesced,
            closed
        };

        enum class wait_result
        {
            notified,
            closed,
            cancelled,
            timed_out,
            failed
        };

      public:
        explicit async_event(net::any_io_executor ex);
        ~async_event();

        async_event(async_event const&) = delete;
        async_event& operator=(async_event const&) = delete;

        async_event(async_event&&) = delete;
        async_event& operator=(async_event&&) = delete;

        /**
         * @brief The executor associated with this event.
         */
        net::any_io_executor get_executor() const;

        /**
         * @brief Notify the event, waking at most one waiter.
         *
         * This function is thread-safe.
         *
         * If the event is already signaled, the notification is
         * coalesced and no additional wakeup is generated.
         */
        notify_result notify_one();

        /**
         * @brief Wake every currently waiting coroutine.
         *
         * If no waiter is present the notification is coalesced into the
         * latched SIGNALED state, exactly like notify_one().
         *
         * This function is thread-safe.
         */
        notify_result notify_all();

        /**
         * @brief Wait for a notification.
         *
         * The notification is consumed by this wait operation.
         *
         * Returns:
         *
         *     notified
         *         A notification was received.
         *
         *     closed
         *         The event was closed.
         *
         *     cancelled
         *         The coroutine's cancellation request interrupted
         *         the wait.
         *
         *     failed
         *         Another error occurred.
         *
         * The function itself does not throw for normal asynchronous
         * cancellation or close().
         */
        net::awaitable<wait_result> wait();

        /**
         * @brief Wait for a notification, or a timeout.
         *
         * Behaves like wait(), but returns wait_result::timed_out if no
         * notification arrives within `timeout`.
         */
        net::awaitable<wait_result> wait_for(std::chrono::steady_clock::duration timeout);

        /**
         * @brief Non-blocking consume of a latched notification.
         *
         * Returns true iff the event was in the SIGNALED state, which is
         * then consumed. Returns false when the event is not signaled or
         * has been closed.
         */
        bool try_wait();

        /**
         * @brief Close the event permanently.
         *
         * close() is idempotent and thread-safe.
         *
         * All currently waiting coroutines are woken with
         * wait_result::closed. Future wait() calls return
         * wait_result::closed.
         *
         * After close():
         *
         *     notify() == notify_result::closed
         */
        void close();

        /**
         * @brief Reopen a closed event so it can be reused.
         *
         * Clears any latched notification and returns the event to the
         * NOT_SIGNALED state. Intended to be called when no wait() is in
         * flight, e.g. between two runs of an owner object. Waiters that were
         * already woken by a preceding close() keep their `closed` result.
         *
         * This function is thread-safe with respect to close() and notify().
         */
        void reset();

        /**
         * @brief Whether the event has been permanently closed.
         *
         * This is only a snapshot.
         *
         * Do not use this function for synchronization.
         */
        bool is_closed() const noexcept;

        /**
         * @brief Whether a notification is currently latched.
         *
         * This is only a snapshot.
         *
         * Do not use this function for synchronization.
         */
        bool is_signaled() const;

      private:
        class impl;
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::util
