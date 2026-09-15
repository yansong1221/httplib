#include "httplib/util/async_event.hpp"
#include <atomic>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

namespace net = httplib::net;
using httplib::util::async_event;

namespace
{
    using wait_result = async_event::wait_result;
    using notify_result = async_event::notify_result;

    // Stops the io_context if the test hangs, so a bug shows up as a failed
    // assertion instead of a 300s ctest timeout.
    void
    arm_watchdog(net::io_context& ioc, std::chrono::milliseconds timeout = 5s)
    {
        auto timer = std::make_shared<net::steady_timer>(ioc);
        timer->expires_after(timeout);
        timer->async_wait(
            [&ioc, timer](boost::system::error_code ec)
            {
                if (!ec)
                {
                    ioc.stop();
                }
            });
    }

    // Stops the io_context after the given delay, used to observe that some
    // waiters are still suspended.
    void
    stop_after(net::io_context& ioc, std::chrono::milliseconds delay)
    {
        auto timer = std::make_shared<net::steady_timer>(ioc);
        timer->expires_after(delay);
        timer->async_wait(
            [&ioc, timer](boost::system::error_code ec)
            {
                if (!ec)
                {
                    ioc.stop();
                }
            });
    }
} // namespace

TEST_CASE("async_event: latches a single notification", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    REQUIRE_FALSE(ev.is_signaled());
    REQUIRE(ev.notify_one() == notify_result::notified);
    REQUIRE(ev.is_signaled());
    REQUIRE(ev.notify_one() == notify_result::coalesced);
    REQUIRE(ev.notify_all() == notify_result::coalesced);
    REQUIRE_FALSE(ev.is_closed());

    std::optional<wait_result> first;
    std::optional<wait_result> second;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            first = co_await ev.wait();
            second = co_await ev.wait_for(50ms);
            ioc.stop();
        },
        net::detached);

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(first == wait_result::notified);
    REQUIRE(second == wait_result::timed_out);
    REQUIRE_FALSE(ev.is_signaled());
}

TEST_CASE("async_event: notify wakes a pending waiter", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    std::optional<wait_result> result;

    auto notifier = std::make_shared<net::steady_timer>(ioc);
    notifier->expires_after(20ms);
    notifier->async_wait(
        [&ev](boost::system::error_code ec)
        {
            if (!ec)
            {
                ev.notify_one();
            }
        });

    net::co_spawn(ioc,
                  ev.wait(),
                  [&](std::exception_ptr, wait_result r)
                  {
                      result = r;
                      ioc.stop();
                  });

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(result == wait_result::notified);
}

TEST_CASE("async_event: notify_one wakes exactly one waiter", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    std::atomic<int> woken { 0 };

    for (int i = 0; i < 2; ++i)
    {
        net::co_spawn(ioc,
                      ev.wait(),
                      [&](std::exception_ptr, wait_result r)
                      {
                          if (r == wait_result::notified)
                          {
                              woken.fetch_add(1);
                          }
                      });
    }

    auto notifier = std::make_shared<net::steady_timer>(ioc);
    notifier->expires_after(50ms);
    notifier->async_wait(
        [&ev](boost::system::error_code ec)
        {
            if (!ec)
            {
                REQUIRE(ev.notify_one() == notify_result::notified);
            }
        });

    stop_after(ioc, 200ms);
    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(woken.load() == 1);
}

TEST_CASE("async_event: notify_all wakes every waiter", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    std::atomic<int> woken { 0 };

    for (int i = 0; i < 4; ++i)
    {
        net::co_spawn(ioc,
                      ev.wait(),
                      [&](std::exception_ptr, wait_result r)
                      {
                          if (r == wait_result::notified)
                          {
                              woken.fetch_add(1);
                          }
                      });
    }

    auto notifier = std::make_shared<net::steady_timer>(ioc);
    notifier->expires_after(50ms);
    notifier->async_wait(
        [&ev](boost::system::error_code ec)
        {
            if (!ec)
            {
                REQUIRE(ev.notify_all() == notify_result::notified);
            }
        });

    stop_after(ioc, 200ms);
    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(woken.load() == 4);
}

TEST_CASE("async_event: try_wait consumes a latched notification", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    REQUIRE_FALSE(ev.try_wait());
    REQUIRE(ev.notify_one() == notify_result::notified);
    REQUIRE(ev.try_wait());
    REQUIRE_FALSE(ev.try_wait());
    REQUIRE_FALSE(ev.is_signaled());
}

TEST_CASE("async_event: close wakes all waiters with closed", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    std::atomic<int> closed_count { 0 };

    for (int i = 0; i < 3; ++i)
    {
        net::co_spawn(ioc,
                      ev.wait(),
                      [&](std::exception_ptr, wait_result r)
                      {
                          if (r == wait_result::closed)
                          {
                              closed_count.fetch_add(1);
                          }
                      });
    }

    auto closer = std::make_shared<net::steady_timer>(ioc);
    closer->expires_after(50ms);
    closer->async_wait(
        [&ev](boost::system::error_code ec)
        {
            if (!ec)
            {
                ev.close();
            }
        });

    stop_after(ioc, 200ms);
    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(closed_count.load() == 3);
    REQUIRE(ev.is_closed());
    REQUIRE(ev.notify_one() == notify_result::closed);
    REQUIRE(ev.notify_all() == notify_result::closed);
    REQUIRE_FALSE(ev.try_wait());
}

TEST_CASE("async_event: close is terminal for wait", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    ev.close();
    ev.close(); // idempotent

    std::optional<wait_result> result;
    net::co_spawn(ioc,
                  ev.wait(),
                  [&](std::exception_ptr, wait_result r)
                  {
                      result = r;
                      ioc.stop();
                  });

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(result == wait_result::closed);
}

TEST_CASE("async_event: wait can be cancelled", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    auto signal = std::make_shared<net::cancellation_signal>();
    std::optional<wait_result> result;

    net::co_spawn(ioc,
                  ev.wait(),
                  net::bind_cancellation_slot(signal->slot(),
                                              [&](std::exception_ptr, wait_result r)
                                              {
                                                  result = r;
                                                  ioc.stop();
                                              }));

    auto canceller = std::make_shared<net::steady_timer>(ioc);
    canceller->expires_after(20ms);
    canceller->async_wait(
        [signal](boost::system::error_code ec)
        {
            if (!ec)
            {
                signal->emit(net::cancellation_type::all);
            }
        });

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(result == wait_result::cancelled);
    REQUIRE_FALSE(ev.is_closed());
    REQUIRE(ev.notify_one() == notify_result::notified);
}

TEST_CASE("async_event: wait_for times out", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    std::optional<wait_result> result;
    net::co_spawn(ioc,
                  ev.wait_for(30ms),
                  [&](std::exception_ptr, wait_result r)
                  {
                      result = r;
                      ioc.stop();
                  });

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(result == wait_result::timed_out);
    REQUIRE_FALSE(ev.is_signaled());
}

TEST_CASE("async_event: wait_for can be cancelled externally", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    auto signal = std::make_shared<net::cancellation_signal>();
    std::optional<wait_result> result;

    net::co_spawn(ioc,
                  ev.wait_for(5s),
                  net::bind_cancellation_slot(signal->slot(),
                                              [&](std::exception_ptr, wait_result r)
                                              {
                                                  result = r;
                                                  ioc.stop();
                                              }));

    auto canceller = std::make_shared<net::steady_timer>(ioc);
    canceller->expires_after(20ms);
    canceller->async_wait(
        [signal](boost::system::error_code ec)
        {
            if (!ec)
            {
                signal->emit(net::cancellation_type::all);
            }
        });

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(result == wait_result::cancelled);
}

TEST_CASE("async_event: wait_for returns notified before timeout", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    std::optional<wait_result> result;

    auto notifier = std::make_shared<net::steady_timer>(ioc);
    notifier->expires_after(20ms);
    notifier->async_wait(
        [&ev](boost::system::error_code ec)
        {
            if (!ec)
            {
                ev.notify_one();
            }
        });

    net::co_spawn(ioc,
                  ev.wait_for(5s),
                  [&](std::exception_ptr, wait_result r)
                  {
                      result = r;
                      ioc.stop();
                  });

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(result == wait_result::notified);
}

TEST_CASE("async_event: destruction wakes a pending waiter with closed", "[async_event]")
{
    net::io_context ioc;
    auto ev = std::make_shared<async_event>(ioc.get_executor());

    std::optional<wait_result> result;

    net::co_spawn(ioc,
                  ev->wait(),
                  [&](std::exception_ptr, wait_result r)
                  {
                      result = r;
                      ioc.stop();
                  });

    auto destroyer = std::make_shared<net::steady_timer>(ioc);
    destroyer->expires_after(20ms);
    destroyer->async_wait(
        [&ev](boost::system::error_code ec)
        {
            if (!ec)
            {
                ev.reset();
            }
        });

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(result == wait_result::closed);
    REQUIRE(ev == nullptr);
}

TEST_CASE("async_event: notify is safe from another thread", "[async_event]")
{
    net::io_context ioc;
    async_event ev(ioc.get_executor());

    std::atomic<bool> woken { false };

    net::co_spawn(ioc,
                  ev.wait(),
                  [&](std::exception_ptr, wait_result r)
                  {
                      woken.store(r == wait_result::notified);
                      ioc.stop();
                  });

    std::thread notifier(
        [&ev]
        {
            std::this_thread::sleep_for(20ms);
            ev.notify_one();
        });

    arm_watchdog(ioc);
    ioc.run();
    notifier.join();

    REQUIRE(woken.load());
}
