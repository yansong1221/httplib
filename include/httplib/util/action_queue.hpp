#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <memory>

namespace httplib::util {
class action_queue : public std::enable_shared_from_this<action_queue>
{
public:
    using act_t = std::function<net::awaitable<void>()>;

    action_queue(const net::any_io_executor& executor);

    void push(act_t&& handler);
    void clear();

    void sync_shutdown(bool cancel_signal = true);
    net::awaitable<void> async_shutdown(bool cancel_signal = true);

private:
    action_queue(const action_queue&)            = delete;
    action_queue& operator=(const action_queue&) = delete;

    class impl;
    std::shared_ptr<impl> impl_;
};
} // namespace httplib::util