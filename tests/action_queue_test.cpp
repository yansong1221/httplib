#include "httplib/util/action_queue.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

TEST_CASE("ActionQueue: push and execute", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    int count = 0;
    bool ok = true;
    bool done = false;
    boost::asio::co_spawn(
        ioc,
        [&]() -> httplib::net::awaitable<void>
        {
            for (int i = 0; i < 3; ++i)
            {
                ok = ok && !aq.push([&]() -> httplib::net::awaitable<void>
                                   {
                                       ++count;
                                       co_return;
                                   });
            }
            co_await aq.async_shutdown();
            done = true;
        },
        boost::asio::detached);

    ioc.run();
    REQUIRE(done);
    REQUIRE(ok);
    REQUIRE(count == 3);
}

TEST_CASE("ActionQueue: sync shutdown drains queued handlers", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    int count = 0;
    REQUIRE(!aq.push([&]() -> httplib::net::awaitable<void>
                     {
                         ++count;
                         co_return;
                     }));
    REQUIRE(!aq.push([&]() -> httplib::net::awaitable<void>
                     {
                         ++count;
                         co_return;
                     }));

    auto fut = aq.shutdown();
    ioc.run();
    REQUIRE(fut.wait_for(1s) == std::future_status::ready);
    REQUIRE(count == 2);
}

TEST_CASE("ActionQueue: push is rejected after shutdown", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    int count = 0;
    boost::system::error_code push_ec;
    boost::asio::co_spawn(
        ioc,
        [&]() -> httplib::net::awaitable<void>
        {
            co_await aq.async_shutdown();
            push_ec = aq.push([&]() -> httplib::net::awaitable<void>
                              {
                                  ++count;
                                  co_return;
                              });
        },
        boost::asio::detached);

    ioc.run();
    REQUIRE(push_ec == boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
    REQUIRE(count == 0);
}

TEST_CASE("ActionQueue: clear drops pending items", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    bool h1_started = false;
    bool h1_release = false;
    int count = 0;
    std::size_t pending_after_clear = 999;
    bool ok = true;
    boost::asio::co_spawn(
        ioc,
        [&]() -> httplib::net::awaitable<void>
        {
            // Park the drain in-flight so handlers queued afterwards stay pending.
            ok = ok && !aq.push([&]() -> httplib::net::awaitable<void>
                               {
                                   h1_started = true;
                                   while (!h1_release)
                                   {
                                       boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
                                       t.expires_after(std::chrono::milliseconds(1));
                                       co_await t.async_wait(boost::asio::use_awaitable);
                                   }
                               });
            while (!h1_started)
            {
                boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
                t.expires_after(std::chrono::milliseconds(1));
                co_await t.async_wait(boost::asio::use_awaitable);
            }

            ok = ok && !aq.push([&]() -> httplib::net::awaitable<void>
                               {
                                   ++count;
                                   co_return;
                               });
            ok = ok && !aq.push([&]() -> httplib::net::awaitable<void>
                               {
                                   ++count;
                                   co_return;
                               });

            aq.clear();
            pending_after_clear = aq.pending();
            h1_release = true;
            co_await aq.async_shutdown();
        },
        boost::asio::detached);

    ioc.run();
    REQUIRE(ok);
    REQUIRE(pending_after_clear == 0);
    REQUIRE(count == 0);
}

TEST_CASE("ActionQueue: double async shutdown is safe", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    bool done = false;
    boost::asio::co_spawn(
        ioc,
        [&]() -> httplib::net::awaitable<void>
        {
            co_await aq.async_shutdown();
            co_await aq.async_shutdown();
            done = true;
        },
        boost::asio::detached);

    ioc.run();
    // Should not hang or crash
    REQUIRE(done);
}

TEST_CASE("ActionQueue: double sync shutdown is safe", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    auto fut1 = aq.shutdown();
    auto fut2 = aq.shutdown();
    ioc.run();
    REQUIRE(fut1.wait_for(1s) == std::future_status::ready);
    REQUIRE(fut2.wait_for(1s) == std::future_status::ready);
}

TEST_CASE("ActionQueue: order is preserved", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    std::vector<int> order;
    bool ok = true;
    boost::asio::co_spawn(
        ioc,
        [&]() -> httplib::net::awaitable<void>
        {
            for (int i = 0; i < 10; ++i)
            {
                ok = ok && !aq.push([&order, i]() -> httplib::net::awaitable<void>
                                   {
                                       order.push_back(i);
                                       co_return;
                                   });
            }
            co_await aq.async_shutdown();
        },
        boost::asio::detached);

    ioc.run();
    REQUIRE(ok);
    REQUIRE(order.size() == 10);
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(order[i] == i);
    }
}

TEST_CASE("ActionQueue: push is rejected when the queue is full", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor(), 2);

    bool parked = false;
    bool release = false;
    int count = 0;
    boost::system::error_code ec[3];
    boost::asio::co_spawn(
        ioc,
        [&]() -> httplib::net::awaitable<void>
        {
            // Park the drain in-flight so subsequent pushes stay pending.
            aq.push([&]() -> httplib::net::awaitable<void>
                    {
                        parked = true;
                        while (!release)
                        {
                            boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
                            t.expires_after(std::chrono::milliseconds(1));
                            co_await t.async_wait(boost::asio::use_awaitable);
                        }
                    });
            while (!parked)
            {
                boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
                t.expires_after(std::chrono::milliseconds(1));
                co_await t.async_wait(boost::asio::use_awaitable);
            }

            ec[0] = aq.push([&]() -> httplib::net::awaitable<void>
                            {
                                ++count;
                                co_return;
                            });
            ec[1] = aq.push([&]() -> httplib::net::awaitable<void>
                            {
                                ++count;
                                co_return;
                            });
            ec[2] = aq.push([&]() -> httplib::net::awaitable<void>
                            {
                                ++count;
                                co_return;
                            });

            release = true;
            co_await aq.async_shutdown();
        },
        boost::asio::detached);

    ioc.run();
    REQUIRE_FALSE(ec[0]);
    REQUIRE_FALSE(ec[1]);
    REQUIRE(ec[2] == boost::system::errc::make_error_code(boost::system::errc::resource_unavailable_try_again));
    REQUIRE(count == 2);
    REQUIRE(aq.pending() == 0);
}

TEST_CASE("ActionQueue: cancel before drain drops queued handlers", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    int count = 0;
    REQUIRE_FALSE(aq.push([&]() -> httplib::net::awaitable<void>
                          {
                              ++count;
                              co_return;
                          }));
    REQUIRE_FALSE(aq.push([&]() -> httplib::net::awaitable<void>
                          {
                              ++count;
                              co_return;
                          }));
    aq.cancel();

    REQUIRE(aq.pending() == 0);
    REQUIRE(aq.push([&]() -> httplib::net::awaitable<void>
                    {
                        ++count;
                        co_return;
                    }) == boost::system::errc::make_error_code(boost::system::errc::operation_canceled));

    auto fut = aq.shutdown();
    ioc.run();
    REQUIRE(fut.wait_for(1s) == std::future_status::ready);
    REQUIRE(count == 0);
}

TEST_CASE("ActionQueue: cancel aborts the running handler and drops pending", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    bool parked = false;
    int count = 0;
    boost::system::error_code handler_ec;
    boost::system::error_code push_ec;
    std::size_t pending_after = 999;

    boost::asio::co_spawn(
        ioc,
        [&]() -> httplib::net::awaitable<void>
        {
            REQUIRE_FALSE(aq.push([&]() -> httplib::net::awaitable<void>
                                   {
                                       parked = true;
                                       for (;;)
                                       {
                                           boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
                                           t.expires_after(std::chrono::milliseconds(1));
                                           co_await t.async_wait(httplib::util::net_awaitable[handler_ec]);
                                           if (handler_ec)
                                           {
                                               break;
                                           }
                                       }
                                   }));
            while (!parked)
            {
                boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
                t.expires_after(std::chrono::milliseconds(1));
                co_await t.async_wait(httplib::util::net_awaitable[handler_ec]);
            }

            REQUIRE_FALSE(aq.push([&]() -> httplib::net::awaitable<void>
                                   {
                                       ++count;
                                       co_return;
                                   }));
            REQUIRE_FALSE(aq.push([&]() -> httplib::net::awaitable<void>
                                   {
                                       ++count;
                                       co_return;
                                   }));

            aq.cancel();
            push_ec = aq.push([&]() -> httplib::net::awaitable<void>
                               {
                                   ++count;
                                   co_return;
                               });
            pending_after = aq.pending();
            co_await aq.async_shutdown();
        },
        boost::asio::detached);

    ioc.run();
    REQUIRE(parked);
    REQUIRE(handler_ec == boost::asio::error::operation_aborted);
    REQUIRE(push_ec == boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
    REQUIRE(pending_after == 0);
    REQUIRE(count == 0);
}