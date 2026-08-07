#pragma once
#include "httplib/config.hpp"
#include "httplib/util/action_queue.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <functional>
#include <memory>
#include <queue>
#include <spdlog/spdlog.h>

namespace httplib::util
{
    class action_queue::impl : public std::enable_shared_from_this<action_queue::impl>
    {
      public:
        impl(net::any_io_executor const& executor) : executor_(executor) {}

        void
        push(act_t&& handler)
        {
            std::unique_lock<std::mutex> lck(que_mutex_);
            if (shutting_down_)
            {
                return;
            }

            que_.push(std::move(handler));

            if (!running_)
            {
                running_ = true;
                lck.unlock();

                net::co_spawn(executor_,
                              perform(),
                              [](std::exception_ptr e)
                              {
                                  if (e)
                                  {
                                      std::rethrow_exception(e);
                                  }
                              });
            }
        }
        void
        clear()
        {
            std::unique_lock<std::mutex> lck(que_mutex_);
            std::queue<act_t> empty;
            std::swap(que_, empty);
        }
        net::any_io_executor
        get_executor() const
        {
            return executor_;
        }

        net::awaitable<void>
        async_shutdown()
        {
            std::unique_lock<std::mutex> lck(que_mutex_);
            if (!shutting_down_.exchange(true))
            {
                co_return;
            }
            lck.unlock();

            boost::system::error_code ec;
            boost::asio::steady_timer wait_timer(executor_);
            for (; running_;)
            {
                wait_timer.expires_after(std::chrono::milliseconds(100));
                co_await wait_timer.async_wait(util::net_awaitable[ec]);
                if (ec)
                {
                    break;
                }
            }
        }
        std::shared_future<void>
        shutdown()
        {
            return boost::asio::co_spawn(
                executor_,
                [this, self = shared_from_this()]() -> net::awaitable<void> { co_return co_await async_shutdown(); },
                boost::asio::use_future);
        }

      private:
        net::awaitable<void>
        perform()
        {
            auto self = shared_from_this();

            for (;;)
            {
                std::unique_lock<std::mutex> lck(que_mutex_);
                if (que_.empty())
                {
                    running_ = false;
                    co_return;
                }

                auto handler = std::move(que_.front());
                que_.pop();
                lck.unlock();

                try
                {
                    co_await handler();
                }
                catch (std::exception const& e)
                {
                    spdlog::error("action_queue handler exception: {}", e.what());
                }
                catch (...)
                {
                    spdlog::error("action_queue handler unknown exception");
                }
            }
        }

      private:
        net::any_io_executor executor_;

        mutable std::mutex que_mutex_;
        std::queue<act_t> que_;

        std::atomic_bool running_ = false;
        std::atomic_bool shutting_down_ = false;
    };
} // namespace httplib::util
