#include "common.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/write_session.hpp"
#include "httplib/server/chunk_writer.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <array>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <type_traits>

namespace
{
    template <typename F>
    void
    run(F&& f)
    {
        net::io_context ioc;
        std::exception_ptr err;
        net::co_spawn(
            ioc,
            [&]() -> net::awaitable<void> { co_await f(ioc); },
            [&](std::exception_ptr e) { err = e; });
        ioc.run();
        if (err)
            std::rethrow_exception(err);
    }

    template <typename Setup, typename Test>
    void
    run_with_server(Setup&& setup, Test&& test)
    {
        net::thread_pool pool{ 1 };
        std::exception_ptr err;
        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void>
            {
                httplib::server::http_server server(pool.get_executor());
                auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
                server.set_logger(std::make_shared<spdlog::logger>("test", null_sink));
                setup(server);
                server.listen("127.0.0.1", 0);
                auto ep = server.local_endpoint();
                server.run();

                co_await test(pool.get_executor(), ep);

                server.stop();
            },
            [&](std::exception_ptr e) { err = e; });
        pool.join();
        if (err)
            std::rethrow_exception(err);
    }
} // namespace

TEST_CASE("client_pool: start/stop lifecycle", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 4});
            p.start();
            auto h = co_await p.async_acquire("127.0.0.1", 80, false, std::chrono::milliseconds(50));
            REQUIRE(h);
            REQUIRE_FALSE(h.has_error());
            p.stop();
            auto h2 = co_await p.async_acquire("127.0.0.1", 80, false, std::chrono::milliseconds(50));
            REQUIRE(h2.has_error());
        });
}

TEST_CASE("client_pool: max_size enforced", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 1});
            p.start();

            auto h1 = co_await p.async_acquire("127.0.0.1", 80, false);
            REQUIRE(h1);
            REQUIRE_FALSE(h1.has_error());

            auto h2 = co_await p.async_acquire("127.0.0.1", 80, false, std::chrono::milliseconds(50));
            REQUIRE(h2.has_error());

            p.stop();
        });
}

TEST_CASE("client_pool: per-host isolation", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 1});
            p.start();

            auto h_a = co_await p.async_acquire("127.0.0.1", 80, false);
            REQUIRE(h_a);
            REQUIRE_FALSE(h_a.has_error());

            auto h_b = co_await p.async_acquire("192.168.0.1", 9999, false, std::chrono::milliseconds(50));
            REQUIRE(h_b);
            REQUIRE_FALSE(h_b.has_error());

            p.stop();
        });
}

TEST_CASE("client_pool: stats reflect correct counts", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 4});
            p.start();

            {
                auto s0 = p.stats("127.0.0.1", 80, false);
                REQUIRE(s0.active == 0);
                REQUIRE(s0.idle == 0);
            }

            auto h1 = co_await p.async_acquire("127.0.0.1", 80, false);
            REQUIRE(h1);
            {
                auto s1 = p.stats("127.0.0.1", 80, false);
                REQUIRE(s1.active == 1);
                REQUIRE(s1.idle == 0);
            }

            auto h2 = co_await p.async_acquire("127.0.0.1", 80, false);
            REQUIRE(h2);
            {
                auto s2 = p.stats("127.0.0.1", 80, false);
                REQUIRE(s2.active == 2);
                REQUIRE(s2.idle == 0);
            }

            h1 = {};
            {
                auto s3 = p.stats("127.0.0.1", 80, false);
                REQUIRE(s3.idle == 1);
            }

            p.stop();
        });
}

TEST_CASE("client_pool: global stats reflect counts", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 4});
            p.start();

            CHECK(p.active_count() == 0);
            CHECK(p.idle_count() == 0);
            CHECK(p.total_count() == 0);

            auto ha = co_await p.async_acquire("127.0.0.1", 80, false);
            auto hb = co_await p.async_acquire("192.168.0.1", 80, false);
            REQUIRE(ha);
            REQUIRE(hb);
            CHECK(p.active_count() == 2);
            CHECK(p.idle_count() == 0);
            CHECK(p.total_count() == 2);

            ha = {};
            CHECK(p.active_count() == 1);
            CHECK(p.idle_count() == 1);
            CHECK(p.total_count() == 2);

            hb = {};
            CHECK(p.active_count() == 0);
            CHECK(p.idle_count() == 2);
            CHECK(p.total_count() == 2);

            p.stop();
        });
}

TEST_CASE("client_pool: idle timeout evicts connection", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(
                ioc.get_executor(),
                { .max_size = 4, .idle_timeout = std::chrono::milliseconds(100), .idle_check_interval = std::chrono::milliseconds(100) });
            p.start();

            {
                auto h = co_await p.async_acquire("127.0.0.1", 80, false, std::chrono::milliseconds(50));
            }

            auto s0 = p.stats("127.0.0.1", 80, false);
            REQUIRE(s0.idle == 1);

            net::steady_timer timer(ioc.get_executor());
            timer.expires_after(std::chrono::milliseconds(300));
            boost::system::error_code ec;
            co_await timer.async_wait(httplib::util::net_awaitable[ec]);

            auto s1 = p.stats("127.0.0.1", 80, false);
            REQUIRE(s1.idle == 0);

            p.stop();
        });
}

TEST_CASE("client_pool: acquire via url string", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 4});
            p.start();

            auto h = co_await p.async_acquire("http://127.0.0.1:80");
            REQUIRE(h);
            REQUIRE(h->port() == 80);

            p.stop();
        });
}

TEST_CASE("client_pool: not started returns error", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 4});
            auto h = co_await p.async_acquire("127.0.0.1", 80, false, std::chrono::milliseconds(50));
            REQUIRE(h.has_error());
        });
}

TEST_CASE("client_pool: acquire timeout", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 1});
            p.start();

            auto h1 = co_await p.async_acquire("127.0.0.1", 80, false);
            REQUIRE(h1);

            auto t0 = std::chrono::steady_clock::now();
            auto h2 = co_await p.async_acquire("127.0.0.1", 80, false, std::chrono::milliseconds(100));
            auto elapsed = std::chrono::steady_clock::now() - t0;
            REQUIRE(h2.has_error());
            REQUIRE(elapsed >= std::chrono::milliseconds(50));

            p.stop();
        });
}

TEST_CASE("client_pool: wait_timeout zero fails fast", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 1});
            p.start();

            auto h1 = co_await p.async_acquire("127.0.0.1", 80, false);
            REQUIRE(h1);

            // Pool is at capacity: wait_timeout == 0 must return timed_out
            // immediately instead of waiting.
            auto t0 = std::chrono::steady_clock::now();
            auto h2 = co_await p.async_acquire(
                "127.0.0.1", 80, false, std::chrono::steady_clock::duration::zero());
            auto elapsed = std::chrono::steady_clock::now() - t0;

            REQUIRE(h2.has_error());
            CHECK(h2.error()
                  == boost::system::errc::make_error_code(boost::system::errc::timed_out));
            CHECK(elapsed < std::chrono::milliseconds(50));

            p.stop();
        });
}

TEST_CASE("client_pool: restart re-arms maintenance", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(
                ioc.get_executor(),
                { .max_size = 4, .idle_timeout = std::chrono::milliseconds(100), .idle_check_interval = std::chrono::milliseconds(100) });
            p.start();

            // stop()/start() in quick succession: the maintenance loop must be
            // re-armed with a fresh timer, and idle eviction must keep working.
            p.stop();
            p.start();

            {
                auto h = co_await p.async_acquire("127.0.0.1", 80, false, std::chrono::milliseconds(50));
                REQUIRE(h);
            }
            CHECK(p.stats("127.0.0.1", 80, false).idle == 1);

            net::steady_timer timer(ioc.get_executor());
            timer.expires_after(std::chrono::milliseconds(300));
            boost::system::error_code ec;
            co_await timer.async_wait(httplib::util::net_awaitable[ec]);

            CHECK(p.stats("127.0.0.1", 80, false).idle == 0);

            p.stop();
        });
}

TEST_CASE("client_pool: waiter wakes up on release", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 1});
            p.start();

            auto h1 = co_await p.async_acquire("127.0.0.1", 80, false);
            REQUIRE(h1);

            std::optional<httplib::client::http_client_pool::client_handle> h2;
            net::co_spawn(
                ioc,
                [&]() -> net::awaitable<void>
                {
                    h2 = co_await p.async_acquire("127.0.0.1", 80, false, std::chrono::seconds(5));
                },
                [](std::exception_ptr)
                {
                });

            net::steady_timer timer(ioc.get_executor());
            timer.expires_after(std::chrono::milliseconds(50));
            boost::system::error_code ec;
            co_await timer.async_wait(httplib::util::net_awaitable[ec]);

            h1 = {};

            net::steady_timer timer2(ioc.get_executor());
            timer2.expires_after(std::chrono::milliseconds(100));
            co_await timer2.async_wait(httplib::util::net_awaitable[ec]);

            REQUIRE(h2.has_value());
            REQUIRE_FALSE(h2->has_error());

            p.stop();
        });
}

TEST_CASE("client_pool: stats for non-existent host returns zeros", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 4});
            p.start();

            auto s = p.stats("10.0.0.1", 9999, false);
            REQUIRE(s.active == 0);
            REQUIRE(s.idle == 0);

            p.stop();
            co_return;
        });
}

TEST_CASE("client_pool: waiter only wakes for matching host", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 1});
            p.start();

            auto h_a = co_await p.async_acquire("127.0.0.1", 80, false);
            REQUIRE(h_a);

            auto h_b_busy = co_await p.async_acquire("192.168.0.1", 9999, false);
            REQUIRE(h_b_busy);

            std::optional<httplib::client::http_client_pool::client_handle> h_b_wait;
            net::co_spawn(
                ioc,
                [&]() -> net::awaitable<void>
                {
                    h_b_wait = co_await p.async_acquire("192.168.0.1", 9999, false, std::chrono::milliseconds(200));
                },
                [](std::exception_ptr)
                {
                });

            net::steady_timer timer(ioc.get_executor());
            timer.expires_after(std::chrono::milliseconds(10));
            boost::system::error_code ec;
            co_await timer.async_wait(httplib::util::net_awaitable[ec]);

            h_a = {};

            net::steady_timer timer2(ioc.get_executor());
            timer2.expires_after(std::chrono::milliseconds(300));
            co_await timer2.async_wait(httplib::util::net_awaitable[ec]);

            REQUIRE(h_b_wait.has_value());
            REQUIRE(h_b_wait->has_error());

            p.stop();
        });
}

TEST_CASE("client_pool: idle reuse cycles", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 2});
            p.start();

            for (int i = 0; i < 5; ++i)
            {
                auto h = co_await p.async_acquire("127.0.0.1", 80, false);
                REQUIRE(h);
            }

            auto s = p.stats("127.0.0.1", 80, false);
            REQUIRE(s.idle == 1);
            REQUIRE(s.active == 0);

            p.stop();
        });
}

TEST_CASE("client_pool: release with unmatched url is safe", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 4});
            p.start();

            auto h_a = co_await p.async_acquire("127.0.0.1", 80, false);
            REQUIRE(h_a);
            REQUIRE(p.stats("127.0.0.1", 80, false).active == 1);

            auto h_b = co_await p.async_acquire("10.0.0.1", 80, false);
            REQUIRE(h_b);

            h_a = {};
            REQUIRE(p.stats("127.0.0.1", 80, false).idle == 1);

            h_b = {};
            REQUIRE(p.stats("10.0.0.1", 80, false).idle == 1);

            p.stop();
        });
}

// ===========================================================================
// URL normalization, max-size semantics, error codes and move semantics
// ===========================================================================

TEST_CASE("client_pool: stats(url) should match acquire(url) normalization", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 4});
            p.start();

            auto h = co_await p.async_acquire("http://127.0.0.1:80");
            REQUIRE(h);
            REQUIRE_FALSE(h.has_error());

            // pool key drops the default port ("http://127.0.0.1"), but stats(url)
            // looks up the raw string, so the two disagree.
            auto sUrl = p.stats("http://127.0.0.1:80");
            auto sHost = p.stats("127.0.0.1", 80, false);
            CHECK(sHost.active == 1);
            CHECK(sUrl.active == sHost.active);

            p.stop();
        });
}

TEST_CASE("client_pool: stats(url) with path/query does not resolve to pool key", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 4});
            p.start();

            auto h = co_await p.async_acquire("http://127.0.0.1:9999");
            REQUIRE(h);
            REQUIRE_FALSE(h.has_error());

            auto s = p.stats("http://127.0.0.1:9999/some/path?x=1");
            CHECK(s.active == 1);

            p.stop();
        });
}

TEST_CASE("client_pool: acquire before start reports operation_canceled", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 4});
            auto h = co_await p.async_acquire("127.0.0.1", 80, false, std::chrono::milliseconds(50));
            REQUIRE(h.has_error());
            CHECK(h.error() == boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
            p.stop();
        });
}

TEST_CASE("client_pool: pool should be movable", "[client_pool]")
{
    CHECK(std::is_move_constructible_v<httplib::client::http_client_pool>);
    CHECK(std::is_move_assignable_v<httplib::client::http_client_pool>);
}

TEST_CASE("client_pool: max_size is enforced per host", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), {.max_size = 1});
            p.start();

            auto ha = co_await p.async_acquire("127.0.0.1", 80, false);
            auto hb = co_await p.async_acquire("192.168.0.1", 80, false);
            REQUIRE(ha);
            REQUIRE(hb);
            CHECK(p.stats("127.0.0.1", 80, false).active == 1);
            CHECK(p.stats("192.168.0.1", 80, false).active == 1);

            p.stop();
        });
}

TEST_CASE("client_pool: max_total caps total across hosts", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(),
                                                {.max_size = 2, .max_total = 2, .idle_timeout = std::chrono::seconds(60)});
            p.start();

            auto ha = co_await p.async_acquire("127.0.0.1", 80, false);
            auto hb = co_await p.async_acquire("192.168.0.1", 80, false);
            REQUIRE(ha);
            REQUIRE(hb);

            auto hc = co_await p.async_acquire("10.0.0.1", 80, false, std::chrono::milliseconds(50));
            REQUIRE(hc.has_error());

            p.stop();
        });
}

TEST_CASE("client_pool: concurrent acquire/release under multithreaded executor", "[client_pool]")
{
    net::thread_pool pool{ 4 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(pool.get_executor(), {.max_size = 8});
            p.start();

            constexpr int kWorkers = 8;
            constexpr int kIterations = 50;

            std::atomic<int> acquired { 0 };
            std::atomic<int> timed_out { 0 };
            std::atomic<int> remaining { kWorkers };

            for (int i = 0; i < kWorkers; ++i)
            {
                net::co_spawn(
                    pool.get_executor(),
                    [&]() -> net::awaitable<void>
                    {
                        for (int j = 0; j < kIterations; ++j)
                        {
                            auto h = co_await p.async_acquire(
                                "127.0.0.1", 80, false, std::chrono::seconds(5));
                            if (h)
                            {
                                ++acquired;
                            }
                            else
                            {
                                ++timed_out;
                            }

                            // Hold the handle briefly so concurrent workers overlap.
                            net::steady_timer t(pool.get_executor());
                            t.expires_after(std::chrono::milliseconds(1));
                            boost::system::error_code ec;
                            co_await t.async_wait(httplib::util::net_awaitable[ec]);
                        }
                        --remaining;
                    },
                    [](std::exception_ptr)
                    {
                    });
            }

            while (remaining.load() > 0)
            {
                net::steady_timer t(pool.get_executor());
                t.expires_after(std::chrono::milliseconds(1));
                boost::system::error_code ec;
                co_await t.async_wait(httplib::util::net_awaitable[ec]);
            }

            CHECK(timed_out.load() == 0);
            CHECK(acquired.load() == kWorkers * kIterations);

            auto s = p.stats("127.0.0.1", 80, false);
            CHECK(s.active == 0);
            CHECK(s.idle <= 8);
            CHECK(s.idle + s.active <= 8);

            p.stop();
        },
        [&](std::exception_ptr e) { err = e; });
    pool.join();
    if (err)
        std::rethrow_exception(err);
}

TEST_CASE("client_pool: reuses a server-closed connection transparently", "[client_pool]")
{
    run_with_server(
        [](httplib::server::http_server& server)
        {
            server.set_read_timeout(std::chrono::milliseconds(200));
            server.router().template set_http_handler<http::verb::get>(
                "/ok",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"); });
        },
        [](net::any_io_executor ex, httplib::tcp::endpoint const& ep) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ex, {.max_size = 1, .idle_timeout = std::chrono::seconds(30)});
            p.start();

            auto host = ep.address().to_string();
            auto port = ep.port();

            {
                auto h = co_await p.async_acquire(host, port, false);
                REQUIRE(h);
                auto r = co_await h->async_get("/ok");
                REQUIRE(r.has_value());
            }
            CHECK(p.stats(host, port, false).idle == 1);

            // The server closes the idle connection after its read timeout.
            net::steady_timer timer(ex);
            timer.expires_after(std::chrono::milliseconds(800));
            boost::system::error_code ec;
            co_await timer.async_wait(httplib::util::net_awaitable[ec]);

            // The pool hands back the same (now-dead) connection; http_client must
            // transparently reconnect instead of failing the request.
            auto h2 = co_await p.async_acquire(host, port, false);
            REQUIRE(h2);
            auto r2 = co_await h2->async_get("/ok");
            CHECK(r2.has_value());

            p.stop();
        });
}

TEST_CASE("client_pool: validate_on_borrow discards dead idle connection", "[client_pool]")
{
    run_with_server(
        [](httplib::server::http_server& server)
        {
            server.set_read_timeout(std::chrono::milliseconds(200));
            server.router().template set_http_handler<http::verb::get>(
                "/ok",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"); });
        },
        [](net::any_io_executor ex, httplib::tcp::endpoint const& ep) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(
                ex, {.max_size = 1, .idle_timeout = std::chrono::seconds(30), .validate_on_borrow = true});
            p.start();

            auto host = ep.address().to_string();
            auto port = ep.port();

            {
                auto h = co_await p.async_acquire(host, port, false);
                REQUIRE(h);
                auto r = co_await h->async_get("/ok");
                REQUIRE(r.has_value());
            }
            CHECK(p.stats(host, port, false).idle == 1);

            // Server closes the idle connection after its read timeout.
            net::steady_timer timer(ex);
            timer.expires_after(std::chrono::milliseconds(800));
            boost::system::error_code ec;
            co_await timer.async_wait(httplib::util::net_awaitable[ec]);

            // validate_on_borrow must discard the dead connection and hand back a
            // fresh working one instead of reusing the dead socket.
            auto h2 = co_await p.async_acquire(host, port, false);
            REQUIRE(h2);
            auto r2 = co_await h2->async_get("/ok");
            CHECK(r2.has_value());

            p.stop();
        });
}

TEST_CASE("http_client: is_alive detects peer close", "[client_pool]")
{
    run_with_server(
        [](httplib::server::http_server& server)
        {
            server.set_read_timeout(std::chrono::milliseconds(200));
            server.router().template set_http_handler<http::verb::get>(
                "/ok",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"); });
        },
        [](net::any_io_executor ex, httplib::tcp::endpoint const& ep) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ex, {.max_size = 1, .idle_timeout = std::chrono::seconds(30)});
            p.start();

            auto host = ep.address().to_string();
            auto port = ep.port();

            auto h = co_await p.async_acquire(host, port, false);
            REQUIRE(h);
            auto r = co_await h->async_get("/ok");
            REQUIRE(r.has_value());
            CHECK(h->is_alive());

            // Server closes the idle connection after its read timeout.
            net::steady_timer timer(ex);
            timer.expires_after(std::chrono::milliseconds(800));
            boost::system::error_code ec;
            co_await timer.async_wait(httplib::util::net_awaitable[ec]);

            CHECK_FALSE(h->is_alive());

            p.stop();
        });
}

TEST_CASE("client_pool: reader survives handle destruction", "[client_pool]")
{
    run_with_server(
        [](httplib::server::http_server& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/stream",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.get_chunk_writer();
                    http::fields headers;
                    headers.set(http::field::content_type, "text/plain");
                    co_await cw->write_header(http::status::ok, headers, false);
                    co_await cw->write_body(net::buffer("ABC"), false);
                });
        },
        [](net::any_io_executor ex, httplib::tcp::endpoint const& ep) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ex, {.max_size = 1, .idle_timeout = std::chrono::seconds(30)});
            p.start();

            auto host = ep.address().to_string();
            auto port = ep.port();

            std::shared_ptr<httplib::client::read_session> reader;
            std::string streamed;
            {
                auto h = co_await p.async_acquire(host, port, false);
                REQUIRE(h);
                auto writer = h->create_writer();
                reader = h->create_reader();

                co_await writer->write_header(http::verb::get, "/stream", {});
                co_await writer->write_body(net::buffer("", 0), false);
                co_await reader->read_header();

                std::array<char, 1> buf;
                auto r = co_await reader->read_body(net::buffer(buf));
                REQUIRE_FALSE(r.has_error());
                REQUIRE(r.value() == 1);
                streamed.append(buf.data(), r.value());
            }
            // handle (and writer) destroyed above; the reader keeps the impl alive.

            std::array<char, 1> buf;
            for (;;)
            {
                auto r = co_await reader->read_body(net::buffer(buf));
                if (r.has_error() || r.value() == 0)
                {
                    break;
                }
                streamed.append(buf.data(), r.value());
            }
            CHECK(streamed.size() >= 3);
            CHECK(streamed.substr(0, 3) == "ABC");

            p.stop();
        });
}

TEST_CASE("client_pool: idle eviction wakes a waiting acquire", "[client_pool]")
{
    run_with_server(
        [](httplib::server::http_server& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ok",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"); });
        },
        [](net::any_io_executor ex, httplib::tcp::endpoint const& ep) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(
                ex,
                { .max_size = 2, .idle_timeout = std::chrono::milliseconds(100), .idle_check_interval = std::chrono::milliseconds(100) });
            p.start();

            auto host = ep.address().to_string();
            auto port = ep.port();

            auto a = co_await p.async_acquire(host, port, false);
            REQUIRE(a);
            auto r0 = co_await a->async_get("/ok");
            REQUIRE(r0.has_value());

            auto b = co_await p.async_acquire(host, port, false);
            REQUIRE(b);

            a.release(); // idle=1, active=1, route total=2=max_size
            CHECK(p.stats(host, port, false).idle == 1);
            CHECK(p.stats(host, port, false).active == 1);

            struct waiter_result
            {
                bool completed = false;
                bool success = false;
                std::chrono::milliseconds elapsed { 0 };
            };
            auto result = std::make_shared<waiter_result>();
            auto start = std::chrono::steady_clock::now();

            net::co_spawn(
                ex,
                [result, start, &p, host, port]() -> net::awaitable<void>
                {
                    try
                    {
                        auto h = co_await p.async_acquire(host, port, false, std::chrono::seconds(2));
                        if (h)
                        {
                            auto r = co_await h->async_get("/ok");
                            result->success = r.has_value();
                        }
                    }
                    catch (...)
                    {
                    }
                    result->elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start);
                    result->completed = true;
                },
                [](std::exception_ptr) {});

            net::steady_timer sleep(ex);
            sleep.expires_after(std::chrono::milliseconds(700));
            boost::system::error_code ec;
            co_await sleep.async_wait(httplib::util::net_awaitable[ec]);

            REQUIRE(result->completed);
            CHECK(result->success);
            CHECK(result->elapsed < std::chrono::milliseconds(1500)); // 被驱逐唤醒，而不是睡满 2s
            b.release();
            p.stop();
        });
}

TEST_CASE("client_pool: stop/start isolates old handles from new pool counts", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), { .max_size = 2 });
            p.start();

            constexpr char const* host = "127.0.0.1";
            uint16_t port = 80;

            auto old = co_await p.async_acquire(host, port, false);
            REQUIRE(old);

            p.stop();
            p.start();

            auto f1 = co_await p.async_acquire(host, port, false);
            REQUIRE(f1);
            auto f2 = co_await p.async_acquire(host, port, false);
            REQUIRE(f2);

            CHECK(p.stats(host, port, false).active == 2);
            CHECK(p.total_count() == 2);

            // 旧 epoch 的 handle 不能再递减新池计数，也不能把旧连接塞回新池。
            old.release();
            CHECK(p.stats(host, port, false).active == 2);
            CHECK(p.total_count() == 2);

            f1.release();
            CHECK(p.stats(host, port, false).active == 1);
            CHECK(p.stats(host, port, false).idle == 1);
            CHECK(p.total_count() == 2);

            f2.release();
            p.stop();
        });
}

TEST_CASE("client_pool: idle_check_interval decouples eviction tick from idle_timeout", "[client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(
                ioc.get_executor(),
                { .max_size = 4, .idle_timeout = std::chrono::milliseconds(100), .idle_check_interval = std::chrono::milliseconds(500) });
            p.start();

            constexpr char const* host = "127.0.0.1";
            uint16_t port = 80;

            {
                auto h = co_await p.async_acquire(host, port, false);
            }
            CHECK(p.stats(host, port, false).idle == 1);

            // idle_timeout 已到，但检查周期尚未到：不应回收。
            net::steady_timer t1(ioc.get_executor());
            t1.expires_after(std::chrono::milliseconds(200));
            boost::system::error_code ec;
            co_await t1.async_wait(httplib::util::net_awaitable[ec]);
            CHECK(p.stats(host, port, false).idle == 1);

            // 检查周期到达后，按 idle_timeout 回收。
            net::steady_timer t2(ioc.get_executor());
            t2.expires_after(std::chrono::milliseconds(600));
            co_await t2.async_wait(httplib::util::net_awaitable[ec]);
            CHECK(p.stats(host, port, false).idle == 0);

            p.stop();
        });
}
