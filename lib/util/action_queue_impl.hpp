#include "httplib/config.hpp"
#include "httplib/util/action_queue.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <cassert>
#include <memory>
#include <mutex>
#include <queue>

namespace httplib::util {

class action_queue::impl : public std::enable_shared_from_this<action_queue::impl>
{
public:
    explicit impl(const net::any_io_executor& executor)
        : executor_(executor)
    {
    }

    void push(act_t&& handler)
    {
        bool need_spawn = false;
        {
            std::lock_guard<std::mutex> lck(mtx_);
            if (shutting_down_)
                return;
            need_spawn = que_.empty();
            que_.push(std::move(handler));
            if (need_spawn)
                running_ = true;
        }
        if (need_spawn) {
            net::co_spawn(
                executor_, perform(), net::bind_cancellation_slot(cs_.slot(), net::detached));
        }
    }

    void clear()
    {
        std::lock_guard<std::mutex> lck(mtx_);
        std::queue<act_t>().swap(que_);
    }

    net::awaitable<void> async_shutdown(bool cancel_signal = true)
    {
        {
            std::lock_guard<std::mutex> lck(mtx_);
            if (shutting_down_)
                co_return;
            shutting_down_ = true;
        }
        if (cancel_signal)
            cs_.emit(boost::asio::cancellation_type::all);

        while (running_) {
            boost::system::error_code ec;
            boost::asio::steady_timer t(executor_);
            t.expires_after(std::chrono::milliseconds(10));
            co_await t.async_wait(util::net_awaitable[ec]);
        }
    }

    std::shared_future<void> shutdown(bool cancel_signal = true)
    {
        return net::co_spawn(
            executor_,
            [this, self = shared_from_this(), cancel_signal]() -> net::awaitable<void> {
                co_await async_shutdown(cancel_signal);
            },
            boost::asio::use_future);
    }

private:
    net::awaitable<void> perform()
    {
        auto self = shared_from_this();

        for (;;) {
            act_t handler;
            {
                std::lock_guard<std::mutex> lck(mtx_);
                if (que_.empty()) {
                    running_ = false;
                    co_return;
                }
                handler = std::move(que_.front());
                que_.pop();
            }

            if (auto cs = co_await net::this_coro::cancellation_state; !cs.cancelled()) {
                try {
                    co_await handler();
                }
                catch (...) {
                    assert(false);
                }
            }
        }
    }

private:
    net::any_io_executor executor_;
    std::mutex mtx_;
    std::queue<act_t> que_;
    boost::asio::cancellation_signal cs_;
    bool running_       = false;
    bool shutting_down_ = false;
};

} // namespace httplib::util
