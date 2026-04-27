#pragma once
#include "httplib/config.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <memory>
#include <queue>

namespace httplib::util {
class action_queue : public std::enable_shared_from_this<action_queue>
{
    using act_t = std::function<net::awaitable<void>()>;

public:
    void push(act_t&& handler)
    {
        if (shutdowning_)
            return;

        std::unique_lock<std::mutex> lck(que_mutex_);
        que_.push(std::move(handler));

        if (!running_) {
            running_ = true;
            lck.unlock();

            net::co_spawn(executor_,
                          perform(),
                          boost::asio::bind_cancellation_slot(cs_.slot(), [](std::exception_ptr e) {
                              if (e) {
                                  std::rethrow_exception(e);
                              }
                          }));
        }
    }
    void clear()
    {
        std::unique_lock<std::mutex> lck(que_mutex_);
        std::queue<act_t> empty;
        std::swap(que_, empty);
    }
    //void shutdown(bool cancel_signal = true)
    //{
    //    if (!running_) {
    //        emit_shutdown_timer();
    //        return;
    //    }

    //    shutdowning_ = true;
    //    if (cancel_signal)
    //        cs_.emit(boost::asio::cancellation_type::all);

    //    std::unique_lock<std::mutex> lck(que_mutex_);
    //    if (!running_ && que_.empty()) {
    //        emit_shutdown_timer();
    //    }
    //}
    net::awaitable<void> async_shutdown(bool cancel_signal = true)
    {
        if (!running_ && shutdowning_) {
            co_return;
        }

        shutdowning_ = true;
        if (cancel_signal)
            cs_.emit(boost::asio::cancellation_type::all);

        boost::system::error_code ec;
        co_await shutdown_timer_.async_wait(net::redirect_error(net::use_awaitable, ec));
    }

protected:
    action_queue(const net::any_io_executor& executor)
        : executor_(executor)
        , shutdown_timer_(executor)
    {
    }
    ~action_queue() { }

public:
    static std::shared_ptr<action_queue> create(const net::any_io_executor& executor)
    {
        struct make_shared_enabler : public action_queue
        {
            make_shared_enabler(const net::any_io_executor& ex)
                : action_queue(ex)
            {
            }
        };
        return std::make_shared<make_shared_enabler>(executor);
    }

private:
    net::awaitable<void> perform()
    {
        auto self = shared_from_this();

        for (auto cs = co_await net::this_coro::cancellation_state;;) {
            std::unique_lock<std::mutex> lck(que_mutex_);
            if (que_.empty() || (bool)cs.cancelled()) {
                running_ = false;
                if (shutdowning_)
                    emit_shutdown_timer();
                co_return;
            }

            auto handler = std::move(que_.front());
            que_.pop();
            lck.unlock();

            co_await handler();
        }
    }
    void emit_shutdown_timer()
    {
        shutdown_timer_.expires_after(std::chrono::steady_clock::duration::max());
        shutdown_timer_.cancel();
    }

private:
    net::any_io_executor executor_;

    mutable std::mutex que_mutex_;
    std::queue<act_t> que_;

    bool running_                 = false;
    std::atomic_bool shutdowning_ = false;

    boost::asio::cancellation_signal cs_;
    net::steady_timer shutdown_timer_;
};
} // namespace httplib::util