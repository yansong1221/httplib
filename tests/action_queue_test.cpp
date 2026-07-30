#include "httplib/util/action_queue.hpp"
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

TEST_CASE("ActionQueue: push and execute", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    int count = 0;
    aq.push([&]() -> httplib::net::awaitable<void> {
        ++count;
        co_return;
    });
    aq.push([&]() -> httplib::net::awaitable<void> {
        ++count;
        co_return;
    });
    aq.push([&]() -> httplib::net::awaitable<void> {
        ++count;
        co_return;
    });

    auto f = aq.shutdown(false);
    ioc.run();
    f.get();

    REQUIRE(count == 3);
}

TEST_CASE("ActionQueue: push after shutdown is ignored", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    int count = 0;
    auto f = aq.shutdown(false);
    ioc.run();
    f.get();

    aq.push([&]() -> httplib::net::awaitable<void> {
        ++count;
        co_return;
    });

    ioc.poll();
    REQUIRE(count == 0);
}

TEST_CASE("ActionQueue: clear drops pending items", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    int count = 0;
    aq.push([&]() -> httplib::net::awaitable<void> {
        ++count;
        co_return;
    });
    aq.push([&]() -> httplib::net::awaitable<void> {
        ++count;
        co_return;
    });
    aq.clear();

    auto f = aq.shutdown(false);
    ioc.run();
    f.get();

    REQUIRE(count == 0);
}

TEST_CASE("ActionQueue: double shutdown is safe", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    auto f1 = aq.shutdown(false);
    auto f2 = aq.shutdown(false);

    ioc.run();
    f1.get();
    f2.get();

    // Should not hang or crash
    SUCCEED();
}

TEST_CASE("ActionQueue: async_shutdown works", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    int count = 0;
    aq.push([&]() -> httplib::net::awaitable<void> {
        ++count;
        co_return;
    });
    aq.push([&]() -> httplib::net::awaitable<void> {
        ++count;
        co_return;
    });

    auto fut = aq.shutdown(false);
    ioc.run();
    fut.get();

    REQUIRE(count == 2);
}

TEST_CASE("ActionQueue: order is preserved", "[action_queue]")
{
    boost::asio::io_context ioc;
    httplib::util::action_queue aq(ioc.get_executor());

    std::vector<int> order;
    for (int i = 0; i < 10; ++i) {
        aq.push([&order, i]() -> httplib::net::awaitable<void> {
            order.push_back(i);
            co_return;
        });
    }

    auto f = aq.shutdown(false);
    ioc.run();
    f.get();

    REQUIRE(order.size() == 10);
    for (int i = 0; i < 10; ++i)
        REQUIRE(order[i] == i);
}
