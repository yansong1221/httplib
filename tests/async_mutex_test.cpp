#include "httplib/util/async_mutex.hpp"
#include <atomic>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

namespace net = httplib::net;
using httplib::util::async_mutex;
using httplib::util::lock_status;

namespace
{
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
} // namespace

TEST_CASE("async_mutex: try_lock acquires and guard releases", "[async_mutex]")
{
    net::io_context ioc;
    async_mutex m(ioc.get_executor());

    auto g = m.try_lock();
    REQUIRE(g.owns_lock());
    REQUIRE(g.status() == lock_status::acquired);
    REQUIRE(m.is_locked());

    auto blocked = m.try_lock();
    REQUIRE_FALSE(blocked.owns_lock());
    REQUIRE(blocked.status() == lock_status::would_block);

    g.reset();
    REQUIRE_FALSE(g.owns_lock());
    REQUIRE_FALSE(m.is_locked());

    auto again = m.try_lock();
    REQUIRE(again.owns_lock());
}

TEST_CASE("async_mutex: serializes concurrent coroutines", "[async_mutex]")
{
    net::io_context ioc;
    async_mutex m(ioc.get_executor());

    constexpr int kWorkers = 8;
    int concurrent = 0;
    int max_concurrent = 0;
    int acquired = 0;
    int released = 0;

    for (int i = 0; i < kWorkers; ++i)
    {
        net::co_spawn(
            ioc,
            [&]() -> net::awaitable<void>
            {
                auto ex = co_await net::this_coro::executor;

                auto g = co_await m.lock();
                if (!g)
                {
                    co_return;
                }

                ++acquired;
                ++concurrent;
                if (concurrent > max_concurrent)
                {
                    max_concurrent = concurrent;
                }

                net::steady_timer timer(ex, 1ms);
                boost::system::error_code ec;
                co_await timer.async_wait(net::redirect_error(net::use_awaitable, ec));

                --concurrent;
                g.reset();
                ++released;
            },
            net::detached);
    }

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(acquired == kWorkers);
    REQUIRE(released == kWorkers);
    REQUIRE(max_concurrent == 1);
    REQUIRE_FALSE(m.is_locked());
}

TEST_CASE("async_mutex: lock_for times out", "[async_mutex]")
{
    net::io_context ioc;
    async_mutex m(ioc.get_executor());

    auto held = m.try_lock();
    REQUIRE(held.owns_lock());

    std::optional<lock_status> status;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto g = co_await m.lock_for(30ms);
            status = g.status();
            ioc.stop();
        },
        net::detached);

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(status == lock_status::timeout);
    REQUIRE(m.is_locked());
}

TEST_CASE("async_mutex: lock_for acquires before timeout", "[async_mutex]")
{
    net::io_context ioc;
    async_mutex m(ioc.get_executor());

    auto held = m.try_lock();
    REQUIRE(held.owns_lock());

    std::optional<lock_status> status;

    auto releaser = std::make_shared<net::steady_timer>(ioc);
    releaser->expires_after(20ms);
    releaser->async_wait(
        [&](boost::system::error_code ec)
        {
            if (!ec)
            {
                held.reset();
            }
        });

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto g = co_await m.lock_for(5s);
            status = g.status();
            ioc.stop();
        },
        net::detached);

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(status == lock_status::acquired);
}

TEST_CASE("async_mutex: lock_for with zero timeout does not wait", "[async_mutex]")
{
    net::io_context ioc;
    async_mutex m(ioc.get_executor());

    std::optional<lock_status> status;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto g = co_await m.lock_for(0ms);
            status = g.status();
            if (g)
            {
                g.reset();
            }
            ioc.stop();
        },
        net::detached);

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(status == lock_status::acquired);
    REQUIRE_FALSE(m.is_locked());
}

TEST_CASE("async_mutex: waiting lock can be cancelled", "[async_mutex]")
{
    net::io_context ioc;
    async_mutex m(ioc.get_executor());

    auto held = m.try_lock();
    REQUIRE(held.owns_lock());

    auto signal = std::make_shared<net::cancellation_signal>();
    std::optional<lock_status> status;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto g = co_await m.lock();
            status = g.status();
            ioc.stop();
        },
        net::bind_cancellation_slot(signal->slot(), net::detached));

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

    REQUIRE(status == lock_status::cancelled);
    REQUIRE(m.is_locked());
    REQUIRE(held.owns_lock());
}

TEST_CASE("async_mutex: close wakes waiters and rejects new locks", "[async_mutex]")
{
    net::io_context ioc;
    async_mutex m(ioc.get_executor());

    auto held = m.try_lock();
    REQUIRE(held.owns_lock());

    std::optional<lock_status> status;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto g = co_await m.lock();
            status = g.status();
            ioc.stop();
        },
        net::detached);

    auto closer = std::make_shared<net::steady_timer>(ioc);
    closer->expires_after(20ms);
    closer->async_wait(
        [&](boost::system::error_code ec)
        {
            if (!ec)
            {
                m.close();
            }
        });

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(status == lock_status::closed);
    REQUIRE(m.is_closed());

    // 已持有锁的 owner 仍可正常释放。
    held.reset();
    REQUIRE_FALSE(m.is_locked());

    REQUIRE(m.try_lock().status() == lock_status::closed);
}

TEST_CASE("async_mutex: destruction wakes a pending waiter", "[async_mutex]")
{
    net::io_context ioc;
    auto m = std::make_shared<async_mutex>(ioc.get_executor());

    auto held = m->try_lock();
    REQUIRE(held.owns_lock());

    std::optional<lock_status> status;

    // 故意只捕获裸指针，验证析构后内部状态仍由等待中的协程持有。
    async_mutex* raw = m.get();
    net::co_spawn(
        ioc,
        [&, raw]() -> net::awaitable<void>
        {
            auto g = co_await raw->lock();
            status = g.status();
            ioc.stop();
        },
        net::detached);

    auto destroyer = std::make_shared<net::steady_timer>(ioc);
    destroyer->expires_after(20ms);
    destroyer->async_wait(
        [&m](boost::system::error_code ec)
        {
            if (!ec)
            {
                m.reset();
            }
        });

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(status == lock_status::closed);
    REQUIRE(m == nullptr);
}

TEST_CASE("async_mutex: guard releases on scope exit", "[async_mutex]")
{
    net::io_context ioc;
    async_mutex m(ioc.get_executor());

    bool locked_inside_scope = false;
    bool reacquired = false;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            {
                auto g = co_await m.lock();
                if (g)
                {
                    locked_inside_scope = m.is_locked();
                }
            }

            auto g = m.try_lock();
            reacquired = g.owns_lock();
            if (g)
            {
                g.reset();
            }
            ioc.stop();
        },
        net::detached);

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(locked_inside_scope);
    REQUIRE(reacquired);
    REQUIRE_FALSE(m.is_locked());
}

TEST_CASE("async_mutex: waiter_count tracks queued waiters", "[async_mutex]")
{
    net::io_context ioc;
    async_mutex m(ioc.get_executor());

    auto held = m.try_lock();
    REQUIRE(held.owns_lock());

    for (int i = 0; i < 3; ++i)
    {
        net::co_spawn(
            ioc,
            [&]() -> net::awaitable<void>
            {
                auto g = co_await m.lock();
                if (g)
                {
                    g.reset();
                }
            },
            net::detached);
    }

    // 让三个协程全部进入等待队列。
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto ex = co_await net::this_coro::executor;
            net::steady_timer timer(ex, 20ms);
            boost::system::error_code ec;
            co_await timer.async_wait(net::redirect_error(net::use_awaitable, ec));
            ioc.stop();
        },
        net::detached);

    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(m.waiter_count() == 3);
    REQUIRE(m.is_locked());

    // 释放后 waiter 依次被唤醒，最终归零。
    held.reset();

    ioc.restart();
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto ex = co_await net::this_coro::executor;
            net::steady_timer timer(ex, 30ms);
            boost::system::error_code ec;
            co_await timer.async_wait(net::redirect_error(net::use_awaitable, ec));
            ioc.stop();
        },
        net::detached);
    arm_watchdog(ioc);
    ioc.run();

    REQUIRE(m.waiter_count() == 0);
    REQUIRE_FALSE(m.is_locked());
}

TEST_CASE("async_mutex: close is safe from another thread", "[async_mutex]")
{
    net::io_context ioc;
    async_mutex m(ioc.get_executor());

    auto held = m.try_lock();
    REQUIRE(held.owns_lock());

    std::optional<lock_status> status;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto g = co_await m.lock();
            status = g.status();
            ioc.stop();
        },
        net::detached);

    std::thread closer(
        [&]
        {
            std::this_thread::sleep_for(20ms);
            m.close();
        });

    arm_watchdog(ioc);
    ioc.run();
    closer.join();

    REQUIRE(status == lock_status::closed);
    held.reset();
    REQUIRE_FALSE(m.is_locked());
}
