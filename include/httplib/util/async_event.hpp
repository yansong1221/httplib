#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
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
     * notify():
     *     - changes NOT_SIGNALED -> SIGNALED
     *     - wakes one waiter if present
     *     - repeated notifications are coalesced
     *
     * wait():
     *     - consumes the SIGNALED state
     *     - suspends if the event is not signaled
     *
     * close():
     *     - permanently closes the event
     *     - wakes all pending waiters
     *     - subsequent notify() calls are rejected
     *     - subsequent wait() calls return operation_aborted
     *
     * Important:
     *
     *     This is NOT a counting semaphore.
     *
     *     notify();
     *     notify();
     *     notify();
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
     *     notify() and close() may be called from arbitrary threads.
     *
     * Waiters should normally be associated with the event's executor.
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
         * @brief Notify the event.
         *
         * This function is thread-safe.
         *
         * If the event is already signaled, the notification is
         * coalesced and no additional wakeup is generated.
         */
        notify_result notify();

        /**
         * @brief Alias for notify().
         */
        notify_result signal()
        {
            return notify();
        }

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
         * @brief Close the event permanently.
         *
         * close() is idempotent and thread-safe.
         *
         * All currently waiting coroutines are woken.
         * Future wait() calls return wait_result::closed.
         *
         * After close():
         *
         *     notify() == notify_result::closed
         */
        void close();

        /**
         * @brief Whether the event has been permanently closed.
         *
         * This is only a snapshot.
         *
         * Do not use this function for synchronization.
         */
        bool is_closed() const noexcept;

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::util