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
    void shutdown(bool cancel_signal = true)
    {
        if (shutdowning_)
            return;

        shutdowning_ = true;
        if (cancel_signal)
            cs_.emit(boost::asio::cancellation_type::all);
    }

protected:
    action_queue(const net::any_io_executor& executor)
        : executor_(executor)
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

        for (;;) {
            std::unique_lock<std::mutex> lck(que_mutex_);
            if (que_.empty()) {
                running_ = false;
                co_return;
            }

            auto handler = std::move(que_.front());
            que_.pop();
            lck.unlock();

            co_await handler();
        }
    }

private:
    net::any_io_executor executor_;

    mutable std::mutex que_mutex_;
    std::queue<act_t> que_;

    bool running_                 = false;
    std::atomic_bool shutdowning_ = false;
    boost::asio::cancellation_signal cs_;
};
} // namespace httplib::util