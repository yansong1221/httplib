#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <future>
#include <memory>

namespace httplib::util
{

    class HTTPLIB_API action_queue
    {
      public:
        using act_t = std::function<net::awaitable<void>()>;

        action_queue(net::any_io_executor const& executor);

        void push(act_t&& handler);
        void clear();

        std::shared_future<void> shutdown(bool cancel_signal = false);
        net::awaitable<void> async_shutdown(bool cancel_signal = false);

      private:
        action_queue(action_queue const&) = delete;
        action_queue& operator=(action_queue const&) = delete;

        class impl;
        std::shared_ptr<impl> impl_;
    };
} // namespace httplib::util