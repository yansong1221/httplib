#pragma once
#include "httplib/config.hpp"
#include "httplib/util/action_queue.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>

namespace httplib::util
{
    class action_queue::impl : public std::enable_shared_from_this<action_queue::impl>
    {
      public:
        impl(net::any_io_executor const& executor, std::size_t max_pending)
            : executor_(executor)
            , max_pending_(max_pending)
        {
        }

        boost::system::error_code
        push(act_t&& handler)
        {
            std::unique_lock<std::mutex> lck(que_mutex_);
            if (shutting_down_)
            {
                return boost::system::errc::make_error_code(boost::system::errc::operation_canceled);
            }
            if (max_pending_ != 0 && que_.size() >= max_pending_)
            {
                return boost::system::errc::make_error_code(boost::system::errc::resource_unavailable_try_again);
            }

            que_.push(std::move(handler));

            if (!running_)
            {
                running_ = true;
                lck.unlock();

                net::co_spawn(
                    executor_,
                    [this, self = shared_from_this()]() -> net::awaitable<void>
                    {
                        try
                        {
                            co_await perform();
                        }
                        catch (...)
                        {
                            std::terminate();
                        }
                    },
                    boost::asio::detached);
            }
            return boost::system::error_code {};
        }
        void
        clear()
        {
            std::queue<act_t> empty;
            {
                std::unique_lock<std::mutex> lck(que_mutex_);
                std::swap(que_, empty);
            }
        }
        std::size_t
        pending() const
        {
            std::unique_lock<std::mutex> lck(que_mutex_);
            return que_.size();
        }
        void
        cancel()
        {
            std::queue<act_t> empty;
            std::unique_lock<std::mutex> lck(que_mutex_);
            shutting_down_ = true;
            if (cur_sig_)
            {
                cur_sig_->emit(boost::asio::cancellation_type::all);
            }
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
            {
                std::unique_lock<std::mutex> lck(que_mutex_);
                shutting_down_ = true;
                if (!running_)
                {
                    co_return;
                }
            }

            boost::system::error_code ec;
            boost::asio::steady_timer wait_timer(executor_);
            while (running_)
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

        net::awaitable<void>
        perform()
        {
            auto self = shared_from_this();

            for (;;)
            {
                co_await net::dispatch(executor_);

                std::unique_lock<std::mutex> lck(que_mutex_);
                if (que_.empty())
                {
                    running_ = false;
                    cur_sig_.reset();
                    co_return;
                }

                auto handler = std::move(que_.front());
                que_.pop();

                auto sig = std::make_shared<boost::asio::cancellation_signal>();
                cur_sig_ = sig;
                lck.unlock();

                try
                {
                    co_await net::co_spawn(
                        executor_,
                        std::move(handler),
                        net::bind_cancellation_slot(sig->slot(), net::use_awaitable));
                }
                catch (boost::system::system_error const& e)
                {
                    // The handler was cancelled: stop promptly.
                    if (e.code() != boost::asio::error::operation_aborted)
                    {
                        throw;
                    }
                    std::unique_lock<std::mutex> stop_lck(que_mutex_);
                    running_ = false;
                    cur_sig_.reset();
                    co_return;
                }

                std::unique_lock<std::mutex> done_lck(que_mutex_);
                cur_sig_.reset();
            }
        }

      private:
        net::any_io_executor executor_;
        std::size_t max_pending_;

        mutable std::mutex que_mutex_;
        std::queue<act_t> que_;
        std::shared_ptr<boost::asio::cancellation_signal> cur_sig_;

        std::atomic_bool running_ = false;
        std::atomic_bool shutting_down_ = false;
    };
} // namespace httplib::util