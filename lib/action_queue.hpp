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
    void push(act_t&& handler)
    {
        net::dispatch(strand_, [this, handler = std::move(handler)]() mutable {
            bool in_process = !que_.empty();
            que_.push(std::move(handler));
            if (in_process)
                return;
            net::co_spawn(strand_, perform(), net::detached);
        });
    }

private:
    net::awaitable<void> perform()
    {
        for (;;) {
            //co_await net::dispatch(strand_);
            if (que_.empty())
                co_return;
            auto&& handler = std::move(que_.front());
            co_await handler();
            //co_await net::dispatch(strand_);
            que_.pop();
        }
    }

private:
    net::strand<net::any_io_executor> strand_;
    std::queue<act_t> que_;
};
} // namespace httplib