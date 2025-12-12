#pragma once
#include "httplib/config.hpp"
#include "httplib/use_awaitable.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <functional>
#include <queue>

namespace httplib {
class action_queue
{
    using act_t = std::function<net::awaitable<void>()>;

public:
    action_queue(const net::any_io_executor& executor)
        : strand_(executor)
    {
    }
    ~action_queue() { shutdown(); }

    void push(act_t&& handler)
    {
        net::dispatch(strand_, [this, handler = std::move(handler)]() mutable {
            if (abort_)
                return;

            que_.push(std::move(handler));
            if (!running_) {
                running_ = true;
                net::co_spawn(strand_.get_inner_executor(), perform(), [](std::exception_ptr e) {
                    if (e)
                        std::rethrow_exception(e);
                });
            }
        });
    }
    void shutdown()
    {
        net::dispatch(strand_, [this]() mutable {
            if (abort_)
                return;
            abort_ = true;
        });
    }

private:
    net::awaitable<void> perform()
    {
        for (; !abort_;) {
            co_await net::dispatch(strand_);
            if (que_.empty()) {
                running_ = false;
                co_return;
            }

            auto handler = std::move(que_.front());
            que_.pop();

            co_await net::dispatch(strand_.get_inner_executor());
            co_await handler();
        }
    }

private:
    net::strand<net::any_io_executor> strand_;
    std::queue<act_t> que_;
    bool running_ = false;
    bool abort_   = false;
};
} // namespace httplib