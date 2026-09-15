#include "common.hpp"
#include <boost/asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>

TEST_CASE("http_server: async_stop waits for the run loop to finish", "[server]")
{
    net::thread_pool pool { 1 };
    std::exception_ptr err;
    bool stop_done = false;
    bool run_ready_on_return = false;

    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            httplib::server::http_server server(pool.get_executor());
            test_common::setup_logger(server);
            server.listen("127.0.0.1", 0);

            auto run_future = server.run();

            // Let async_run() claim the running flag and start accepting.
            co_await net::post(pool.get_executor(), net::use_awaitable);

            co_await server.async_stop();
            stop_done = true;

            run_ready_on_return = (run_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        },
        [&](std::exception_ptr e) { err = e; });

    pool.join();
    if (err)
    {
        std::rethrow_exception(err);
    }
    REQUIRE(stop_done);
    REQUIRE(run_ready_on_return);
}

TEST_CASE("http_server: can be run again after async_stop", "[server]")
{
    net::thread_pool pool { 1 };
    std::exception_ptr err;
    int completed_runs = 0;

    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            httplib::server::http_server server(pool.get_executor());
            test_common::setup_logger(server);

            for (int i = 0; i < 2; ++i)
            {
                server.listen("127.0.0.1", 0);
                auto run_future = server.run();

                // Let async_run() claim the running flag and start accepting.
                co_await net::post(pool.get_executor(), net::use_awaitable);

                co_await server.async_stop();
                if (run_future.wait_for(std::chrono::seconds(1)) != std::future_status::ready)
                {
                    co_return;
                }
                ++completed_runs;
            }
        },
        [&](std::exception_ptr e) { err = e; });

    pool.join();
    if (err)
    {
        std::rethrow_exception(err);
    }
    REQUIRE(completed_runs == 2);
}
