#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>

namespace httplib::util
{

    /// Serialises fire-and-forget coroutine handlers on a single executor.
    ///
    /// `push()` may be called from any thread. Handlers run one at a time in
    /// FIFO order. The queue can be drained gracefully with `shutdown()` /
    /// `async_shutdown()`, or torn down immediately with `cancel()`.
    class HTTPLIB_API action_queue
    {
      public:
        using act_t = std::function<net::awaitable<void>()>;

        /// Called with the current exception whenever a handler throws. Without
        /// one, handler exceptions are swallowed so a misbehaving handler
        /// cannot take the worker (or the process) down.
        using error_handler_t = std::function<void(std::exception_ptr)>;

        /// @param max_pending Maximum number of queued (not yet running)
        ///        handlers; `push` fails with `resource_unavailable_try_again`
        ///        once the limit is reached. Defaults to unbounded.
        /// @param on_error Optional handler-exception sink.
        action_queue(net::any_io_executor const& executor,
                     std::size_t max_pending = std::numeric_limits<std::size_t>::max(),
                     error_handler_t on_error = {});
        ~action_queue();

        /// Enqueue a handler. Returns a non-success `boost::system::error_code`
        /// if the queue is already shutting down or would exceed `max_pending`
        /// pending handlers.
        boost::system::error_code push(act_t handler);

        /// Drop all pending handlers. The handler currently running, if any,
        /// is unaffected.
        void clear();

        /// Cancel all pending handlers, abort the handler currently running and
        /// stop the queue permanently. Subsequent `push` calls fail with
        /// `operation_canceled`.
        void cancel();

        /// Number of handlers currently pending in the queue.
        std::size_t pending() const;

        /// Stop accepting work and wait, asynchronously, until the worker has
        /// drained the remaining handlers and exited.
        net::awaitable<void> async_shutdown();

        /// Blocking variant of `async_shutdown()`; the returned future becomes
        /// ready once the queue has drained and shut down.
        std::shared_future<void> shutdown();

      private:
        action_queue(action_queue const&) = delete;
        action_queue& operator=(action_queue const&) = delete;

        class impl;
        std::shared_ptr<impl> impl_;
    };
} // namespace httplib::util
