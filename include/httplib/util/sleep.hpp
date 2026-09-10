#pragma once
#include "httplib/util/use_awaitable.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>

namespace httplib::util
{
    inline static boost::asio::awaitable<boost::system::error_code>
    sleep(boost::asio::steady_timer& timer, std::chrono::steady_clock::duration const& duration)
    {
        timer.expires_after(duration);
        boost::system::error_code ec;
        co_await timer.async_wait(net_awaitable[ec]);
        co_return ec;
    }

    inline static boost::asio::awaitable<boost::system::error_code>
    sleep(std::chrono::steady_clock::duration const& duration)
    {
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
        co_return co_await sleep(timer, duration);
    }
} // namespace httplib::util
