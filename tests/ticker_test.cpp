#include "httplib/util/ticker.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>

using namespace std::chrono_literals;

namespace
{
    class test_ticker : public httplib::util::ticker
    {
      public:
        using ticker::ticker;

        int start_calls = 0;
        int tick_calls = 0;
        int stop_calls = 0;

        bool start_ok = true;
        int stop_after_tick = 0; // 0 = never stop on its own
        int throw_at_tick = 0;   // 0 = never throw

      protected:
        boost::asio::awaitable<bool>
        on_start() override
        {
            ++start_calls;
            co_return start_ok;
        }

        boost::asio::awaitable<bool>
        on_tick() override
        {
            ++tick_calls;
            if (throw_at_tick > 0 && tick_calls == throw_at_tick)
            {
                throw boost::system::system_error(
                    boost::system::errc::make_error_code(boost::system::errc::io_error));
            }
            co_return !(stop_after_tick > 0 && tick_calls >= stop_after_tick);
        }

        boost::asio::awaitable<void>
        on_stop() override
        {
            ++stop_calls;
            co_return;
        }
    };

    class parking_ticker : public httplib::util::ticker
    {
      public:
        using ticker::ticker;

        bool tick_started = false;
        bool tick_completed = false;
        int stop_calls = 0;

      protected:
        boost::asio::awaitable<bool>
        on_tick() override
        {
            tick_started = true;
            boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
            t.expires_after(10s);
            boost::system::error_code ec;
            co_await t.async_wait(httplib::util::net_awaitable[ec]);
            tick_completed = !ec;
            co_return true;
        }

        boost::asio::awaitable<void>
        on_stop() override
        {
            ++stop_calls;
            co_return;
        }
    };
} // namespace

TEST_CASE("Ticker: ticks then self-stops when on_tick returns false", "[ticker]")
{
    boost::asio::io_context ioc;
    auto t = std::make_shared<test_ticker>(ioc.get_executor(), 1ms);
    t->stop_after_tick = 3;

    REQUIRE_FALSE(t->is_running());
    t->start();
    REQUIRE(t->is_running());

    ioc.run();

    REQUIRE(t->start_calls == 1);
    REQUIRE(t->tick_calls == 3);
    REQUIRE(t->stop_calls == 1);
    REQUIRE_FALSE(t->is_running());
}

TEST_CASE("Ticker: stop() halts the loop and runs on_stop", "[ticker]")
{
    boost::asio::io_context ioc;
    auto t = std::make_shared<test_ticker>(ioc.get_executor(), 1ms);

    t->start();

    boost::asio::steady_timer stopper(ioc);
    stopper.expires_after(20ms);
    stopper.async_wait([&](boost::system::error_code) { t->stop(); });

    ioc.run();

    REQUIRE(t->tick_calls >= 1);
    REQUIRE(t->stop_calls == 1);
    REQUIRE_FALSE(t->is_running());
}

TEST_CASE("Ticker: start is idempotent while running", "[ticker]")
{
    boost::asio::io_context ioc;
    auto t = std::make_shared<test_ticker>(ioc.get_executor(), 1ms);

    t->start();
    REQUIRE(t->is_running());

    boost::asio::steady_timer stopper(ioc);
    stopper.expires_after(20ms);
    stopper.async_wait([&](boost::system::error_code)
    {
        // A second start() while running must be a no-op.
        t->start();
        t->stop();
    });

    ioc.run();

    REQUIRE(t->start_calls == 1);
    REQUIRE(t->tick_calls >= 1);
    REQUIRE(t->stop_calls == 1);
    REQUIRE_FALSE(t->is_running());
}

TEST_CASE("Ticker: can be restarted after stop", "[ticker]")
{
    boost::asio::io_context ioc;
    auto t = std::make_shared<test_ticker>(ioc.get_executor(), 1ms);
    t->stop_after_tick = 2;

    t->start();
    ioc.run();
    REQUIRE(t->tick_calls == 2);
    REQUIRE(t->stop_calls == 1);
    REQUIRE_FALSE(t->is_running());

    t->stop_after_tick = 3;
    t->start();
    REQUIRE(t->is_running());

    ioc.restart();
    ioc.run();

    REQUIRE(t->start_calls == 2);
    REQUIRE(t->tick_calls == 3);
    REQUIRE(t->stop_calls == 2);
    REQUIRE_FALSE(t->is_running());
}

TEST_CASE("Ticker: on_start returning false prevents ticks but runs on_stop", "[ticker]")
{
    boost::asio::io_context ioc;
    auto t = std::make_shared<test_ticker>(ioc.get_executor(), 1ms);
    t->start_ok = false;

    t->start();
    ioc.run();

    REQUIRE(t->start_calls == 1);
    REQUIRE(t->tick_calls == 0);
    REQUIRE(t->stop_calls == 1);
    REQUIRE_FALSE(t->is_running());
}

TEST_CASE("Ticker: on_tick exception stops the loop and runs on_stop", "[ticker]")
{
    boost::asio::io_context ioc;
    auto t = std::make_shared<test_ticker>(ioc.get_executor(), 1ms);
    t->throw_at_tick = 1;

    t->start();
    ioc.run();

    REQUIRE(t->tick_calls == 1);
    REQUIRE(t->stop_calls == 1);
    REQUIRE_FALSE(t->is_running());
}

TEST_CASE("Ticker: set_interval controls the tick period", "[ticker]")
{
    boost::asio::io_context ioc;
    auto t = std::make_shared<test_ticker>(ioc.get_executor(), 1ms);
    t->set_interval(500ms);

    t->start();

    // Stop long before the first (500ms) tick would fire.
    boost::asio::steady_timer stopper(ioc);
    stopper.expires_after(50ms);
    stopper.async_wait([&](boost::system::error_code) { t->stop(); });

    ioc.run();

    REQUIRE(t->tick_calls == 0);
    REQUIRE(t->stop_calls == 1);
    REQUIRE_FALSE(t->is_running());
}

TEST_CASE("Ticker: stop() interrupts an in-flight on_tick", "[ticker]")
{
    boost::asio::io_context ioc;
    auto t = std::make_shared<parking_ticker>(ioc.get_executor(), 1ms);

    t->start();

    boost::asio::steady_timer stopper(ioc);
    stopper.expires_after(20ms);
    stopper.async_wait([&](boost::system::error_code) { t->stop(); });

    auto begin = std::chrono::steady_clock::now();
    ioc.run();
    auto elapsed = std::chrono::steady_clock::now() - begin;

    REQUIRE(t->tick_started);
    REQUIRE_FALSE(t->tick_completed);
    REQUIRE(t->stop_calls == 1);
    REQUIRE(elapsed < 5s);
}
