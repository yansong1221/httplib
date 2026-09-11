#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>

namespace httplib::util
{

    class HTTPLIB_API action_queue
    {
      public:
        using act_t = std::function<net::awaitable<void>()>;

        action_queue(net::any_io_executor const& executor, std::size_t max_pending = 0);

        /// Enqueue a handler. Returns a non-success `boost::system::error_code`
        /// if the queue is already shutting down or would exceed `max_pending`
        /// pending handlers.
        boost::system::error_code push(act_t&& handler);
        void clear();

        /// Number of handlers currently pending in the queue.
        std::size_t pending() const;

        /// Synchronously wait until the queue is shut down (drains remaining handlers).
        std::shared_future<void> shutdown();
        net::awaitable<void> async_shutdown();

      private:
        action_queue(action_queue const&) = delete;
        action_queue& operator=(action_queue const&) = delete;

        class impl;
        std::shared_ptr<impl> impl_;
    };
} // namespace httplib::util