#include "common.hpp"
#include "httplib/server/middleware/auth.hpp"
#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/middleware/rate_limit.hpp"
#include "httplib/server/response.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace mw = httplib::server::middleware;
namespace net = httplib::net;
using test_common::run;
using test_common::setup_logger;

namespace
{

    template <typename Setup, typename Test>
    void
    run_with_ep(Setup&& setup, Test&& test)
    {
        net::thread_pool pool { 1 };
        std::exception_ptr err;

        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void>
            {
                httplib::server::http_server server(pool.get_executor());
                setup_logger(server);
                setup(server);
                server.listen("127.0.0.1", 0);
                auto ep = server.local_endpoint();
                server.run();

                httplib::client::http_client client(pool.get_executor(), ep.address().to_string(), ep.port());
                client.set_timeout(std::chrono::seconds(5));

                co_await test(client, ep, pool);

                client.close();
                server.stop();
            },
            [&](std::exception_ptr e) { err = e; });

        pool.join();
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

} // namespace

// ===== middleware execution order =====

TEST_CASE("Global middleware: execution order with route middleware", "[middleware]")
{
    struct order_mw
    {
        std::string name;
        std::shared_ptr<std::vector<std::string>> order;
        bool
        before(httplib::server::request&, httplib::server::response&)
        {
            order->push_back(name + "_before");
            return true;
        }
        bool
        after(httplib::server::request&, httplib::server::response&)
        {
            order->push_back(name + "_after");
            return true;
        }
    };

    auto order = std::make_shared<std::vector<std::string>>();
    order_mw global { "global", order };
    order_mw route { "route", order };

    run(
        [&](auto& server)
        {
            server.router().use(global);
            server.router().template set_http_handler<httplib::method::get>(
                "/order",
                [order](httplib::server::request&, httplib::server::response& resp)
                {
                    order->push_back("handler");
                    resp.set_string_content("ok"sv, "text/plain"sv);
                },
                route);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/order"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(*order
                    == std::vector<std::string> { "global_before",
                                                  "route_before",
                                                  "handler",
                                                  "route_after",
                                                  "global_after" });
            co_return;
        });
}

TEST_CASE("Global middleware: applies to all routes", "[middleware]")
{
    mw::basic_auth_middleware auth([](std::string_view u, std::string_view p)
                                   { return u == "admin" && p == "secret"; });

    run(
        [&](auto& server)
        {
            server.router().use(auth);

            server.router().template set_http_handler<httplib::method::get>(
                "/public",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); });
            server.router().template set_http_handler<httplib::method::get>(
                "/private",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret"sv, "text/plain"sv); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::authorization, "Basic YWRtaW46c2VjcmV0");
            httplib::client::request req1(httplib::method::get, "/public", hdrs);
            auto resp1 = UNWRAP(co_await client.async_send_request(req1));
            REQUIRE(resp1.result() == httplib::status::ok);

            httplib::client::request req2(httplib::method::get, "/private", hdrs);
            auto resp2 = UNWRAP(co_await client.async_send_request(req2));
            REQUIRE(resp2.result() == httplib::status::ok);

            auto resp3 = UNWRAP(co_await client.async_get("/public"));
            REQUIRE(resp3.result() == httplib::status::unauthorized);
            co_return;
        });
}

TEST_CASE("Global middleware: cors_middleware via use()", "[middleware]")
{
    auto cors = mw::cors_middleware {}.allow_origin("x");

    run(
        [&](auto& server)
        {
            server.router().use(cors);

            server.router().template set_http_handler<httplib::method::get>(
                "/gc",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/gc"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(std::string(resp[httplib::field::access_control_allow_origin]) == "x");
            co_return;
        });
}

// ===== cors_middleware =====

TEST_CASE("cors_middleware: allows request without Origin", "[middleware]")
{
    mw::cors_middleware cors;

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/data",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                cors);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/data"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
            REQUIRE_FALSE(resp["Access-Control-Allow-Methods"].empty());
            co_return;
        });
}

TEST_CASE("cors_middleware: OPTIONS preflight is short-circuited", "[middleware]")
{
    mw::cors_middleware cors;

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::options>(
                "/data",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("should-not-reach"sv, "text/plain"sv); },
                cors);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_options("/data"));
            REQUIRE(resp.result() == httplib::status::no_content);
            REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
            co_return;
        });
}

TEST_CASE("cors_middleware: custom origin and credentials", "[middleware]")
{
    auto cors = mw::cors_middleware().allow_origin("https://example.com").allow_credentials(true).max_age(3600);

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/data",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                std::move(cors));
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/data"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp["Access-Control-Allow-Origin"] == "https://example.com");
            REQUIRE(resp["Access-Control-Allow-Credentials"] == "true");
            REQUIRE(resp["Access-Control-Max-Age"] == "3600");
            co_return;
        });
}

TEST_CASE("cors_middleware: allow_origins with multiple origins", "[middleware]")
{
    mw::cors_middleware cors;
    cors.allow_origins({ "https://a.com", "https://b.com" });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/cors_middleware-multi",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("cors_middleware-data"sv, "text/plain"sv); },
                cors);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::origin, "https://a.com");
            httplib::client::request req(httplib::method::get, "/cors_middleware-multi", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "cors_middleware-data");
            co_return;
        });
}

TEST_CASE("cors_middleware: allow_methods custom", "[middleware]")
{
    mw::cors_middleware cors;
    cors.allow_methods({ "PUT", "PATCH" });
    cors.allow_origin("https://x.com");

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::options>(
                "/cors_middleware-methods",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_empty_content(httplib::status::no_content); },
                cors);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::origin, "https://x.com");
            httplib::client::request req(httplib::method::options, "/cors_middleware-methods", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::no_content);
            auto methods = std::string(resp["Access-Control-Allow-Methods"]);
            REQUIRE(methods.find("PUT") != std::string::npos);
            REQUIRE(methods.find("PATCH") != std::string::npos);
            co_return;
        });
}

// ===== Basic Auth =====

TEST_CASE("Basic Auth: valid credentials pass through", "[middleware]")
{
    mw::basic_auth_middleware auth([](std::string_view u, std::string_view p) { return u == "user" && p == "pass"; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/secret",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret-data"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::authorization, "Basic dXNlcjpwYXNz");

            httplib::client::request req(httplib::method::get, "/secret", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "secret-data");
            co_return;
        });
}

TEST_CASE("Basic Auth: invalid credentials return 401", "[middleware]")
{
    mw::basic_auth_middleware auth([](std::string_view u, std::string_view p) { return u == "user" && p == "pass"; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/secret",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret-data"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::authorization, "Basic dXNlcjp3cm9uZw==");

            httplib::client::request req(httplib::method::get, "/secret", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::unauthorized);
            co_return;
        });
}

TEST_CASE("Basic Auth: missing header returns 401", "[middleware]")
{
    mw::basic_auth_middleware auth([](std::string_view, std::string_view) { return true; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/secret",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret-data"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/secret"));
            REQUIRE(resp.result() == httplib::status::unauthorized);
            REQUIRE_FALSE(std::string(resp[httplib::field::www_authenticate]).empty());
            co_return;
        });
}

// ===== Bearer Auth =====

TEST_CASE("Bearer Auth: valid token passes through", "[middleware]")
{
    mw::bearer_auth_middleware auth([](std::string_view t) { return t == "abc-123"; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/token-area",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::authorization, "Bearer abc-123");

            httplib::client::request req(httplib::method::get, "/token-area", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::ok);
            co_return;
        });
}

TEST_CASE("Bearer Auth: invalid token returns 401", "[middleware]")
{
    mw::bearer_auth_middleware auth([](std::string_view t) { return t == "abc-123"; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/token-area",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::authorization, "Bearer wrong-token");

            httplib::client::request req(httplib::method::get, "/token-area", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::unauthorized);
            co_return;
        });
}

TEST_CASE("Bearer Auth: missing header returns 401", "[middleware]")
{
    mw::bearer_auth_middleware auth([](std::string_view) { return true; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/bearer-missing",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/bearer-missing"));
            REQUIRE(resp.result() == httplib::status::unauthorized);
            co_return;
        });
}

TEST_CASE("Bearer Auth: non-Bearer scheme returns 401", "[middleware]")
{
    mw::bearer_auth_middleware auth([](std::string_view) { return true; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/bearer-scheme",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::authorization, "Digest xxx");
            httplib::client::request req(httplib::method::get, "/bearer-scheme", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::unauthorized);
            co_return;
        });
}

// ===== Rate Limit =====

TEST_CASE("Rate Limit: allows requests within limit", "[middleware]")
{
    mw::rate_limit_middleware limiter(10, std::chrono::seconds(60));

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/limited",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                limiter);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            for (int i = 0; i < 5; ++i)
            {
                auto resp = UNWRAP(co_await client.async_get("/limited"));
                REQUIRE(resp.result() == httplib::status::ok);
            }
            co_return;
        });
}

TEST_CASE("Rate Limit: blocks after exceeding limit", "[middleware]")
{
    mw::rate_limit_middleware limiter(3, std::chrono::seconds(60));

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/limited",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                limiter);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            for (int i = 0; i < 3; ++i)
            {
                auto resp = UNWRAP(co_await client.async_get("/limited"));
                REQUIRE(resp.result() == httplib::status::ok);
            }

            auto resp = UNWRAP(co_await client.async_get("/limited"));
            REQUIRE(resp.result() == httplib::status::too_many_requests);
            REQUIRE_FALSE(std::string(resp["Retry-After"]).empty());
            co_return;
        });
}

TEST_CASE("Rate Limit: shared instance across routes", "[middleware]")
{
    auto limiter = mw::rate_limit_middleware(2, std::chrono::seconds(60));

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/a",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("a"sv, "text/plain"sv); },
                limiter);

            server.router().template set_http_handler<httplib::method::get>(
                "/b",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("b"sv, "text/plain"sv); },
                limiter);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            UNWRAP(co_await client.async_get("/a"));
            UNWRAP(co_await client.async_get("/b"));

            auto resp = UNWRAP(co_await client.async_get("/a"));
            REQUIRE(resp.result() == httplib::status::too_many_requests);
            co_return;
        });
}

TEST_CASE("Rate Limit: shared limits apply across routes", "[middleware]")
{
    auto limiter = std::make_shared<mw::rate_limit_middleware>(2, std::chrono::seconds(60));

    run_with_ep(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/rl-ip",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                *limiter);
        },
        [&](auto& client, auto& ep, net::thread_pool& pool) -> net::awaitable<void>
        {
            UNWRAP(co_await client.async_get("/rl-ip"));
            UNWRAP(co_await client.async_get("/rl-ip"));
            auto blocked = co_await client.async_get("/rl-ip");
            REQUIRE(blocked.has_value());
            REQUIRE(blocked->result() == httplib::status::too_many_requests);

            auto client2 = std::make_unique<httplib::client::http_client>(pool.get_executor(),
                                                                          ep.address().to_string(),
                                                                          ep.port());
            client2->set_timeout(std::chrono::seconds(5));
            auto resp2 = co_await client2->async_get("/rl-ip");
            REQUIRE(resp2.has_value());
            REQUIRE(resp2->result() == httplib::status::too_many_requests);
            client2->close();
            co_return;
        });
}

TEST_CASE("Rate Limit: idle buckets are reclaimed", "[middleware]")
{
    // 窗口很长，所以计数不会自然过期；桶的回收只能来自 idle_expiration。
    mw::rate_limit_middleware limiter(1, std::chrono::seconds(60));
    limiter.idle_expiration(std::chrono::milliseconds(50));

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/limited",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                limiter);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            REQUIRE(limiter.tracked_clients() == 0);

            auto first = UNWRAP(co_await client.async_get("/limited"));
            REQUIRE(first.result() == httplib::status::ok);
            REQUIRE(limiter.tracked_clients() == 1);

            auto blocked = UNWRAP(co_await client.async_get("/limited"));
            REQUIRE(blocked.result() == httplib::status::too_many_requests);
            REQUIRE(limiter.tracked_clients() == 1);

            // 空闲超过 idle_expiration 后，桶应被回收并重建，计数随之归零。
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            auto after_idle = UNWRAP(co_await client.async_get("/limited"));
            REQUIRE(after_idle.result() == httplib::status::ok);
            REQUIRE(limiter.tracked_clients() == 1);
            co_return;
        });
}

TEST_CASE("Rate Limit: tracked client count stays bounded", "[middleware]")
{
    mw::rate_limit_middleware limiter(100, std::chrono::seconds(60));
    limiter.max_tracked_clients(4);

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/limited",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                limiter);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            for (int i = 0; i < 20; ++i)
            {
                auto resp = UNWRAP(co_await client.async_get("/limited"));
                REQUIRE(resp.result() == httplib::status::ok);
                // 上限之内（这里是单 IP，但断言对任何 IP 数量都成立）。
                REQUIRE(limiter.tracked_clients() <= 4);
            }
            co_return;
        });
}

TEST_CASE("Rate Limit: max_tracked_clients does not disturb counting", "[middleware]")
{
    // 容量上限为 1，且该 IP 的桶已存在：仍应正常计数并在超限时拦截。
    mw::rate_limit_middleware limiter(2, std::chrono::seconds(60));
    limiter.max_tracked_clients(1);

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/limited",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                limiter);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto first = UNWRAP(co_await client.async_get("/limited"));
            REQUIRE(first.result() == httplib::status::ok);
            auto second = UNWRAP(co_await client.async_get("/limited"));
            REQUIRE(second.result() == httplib::status::ok);

            auto third = UNWRAP(co_await client.async_get("/limited"));
            REQUIRE(third.result() == httplib::status::too_many_requests);
            co_return;
        });
}

// 以下用例用 X-Forwarded-For 模拟不同客户端 IP：真实连接都来自 127.0.0.1，
// 只有把 127.0.0.1 配成可信代理后，get_client_ip() 才会采信 XFF。默认不配置时
// XFF 一律被忽略（见 client_ip_test.cpp）。

TEST_CASE("Rate Limit: new clients are still counted once the table is full", "[middleware]")
{
    // 默认策略 evict_oldest。桶满时如果放行不计数，IP 轮换的攻击者就能完全绕过
    // 限流，限流恰好在最需要它的时刻失效——这里锁死「桶满也要计数」。
    mw::rate_limit_middleware limiter(2, std::chrono::seconds(60));
    limiter.max_tracked_clients(2);

    run(
        [&](auto& server)
        {
            server.set_trusted_proxies({ "127.0.0.1" });
            server.router().template set_http_handler<httplib::method::get>(
                "/limited",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                limiter);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto call = [&](std::string_view ip, httplib::status expected) -> net::awaitable<void>
            {
                auto hdrs = httplib::headers();
                hdrs.set("X-Forwarded-For", std::string(ip));
                httplib::client::request req(httplib::method::get, "/limited", hdrs);
                auto resp = UNWRAP(co_await client.async_send_request(req));
                REQUIRE(resp.result() == expected);
                co_return;
            };

            // 两个客户端把桶表占满。
            co_await call("10.0.0.1", httplib::status::ok);
            co_await call("10.0.0.2", httplib::status::ok);
            REQUIRE(limiter.tracked_clients() == 2);

            // 第三个客户端挤掉最久未访问的桶，并且**照样计入配额**。
            co_await call("10.0.0.3", httplib::status::ok);
            co_await call("10.0.0.3", httplib::status::ok);
            co_await call("10.0.0.3", httplib::status::too_many_requests);
            REQUIRE(limiter.tracked_clients() <= 2);
        });
}

TEST_CASE("Rate Limit: evict_oldest keeps the hottest client tracked", "[middleware]")
{
    mw::rate_limit_middleware limiter(2, std::chrono::seconds(60));
    limiter.max_tracked_clients(2);

    run(
        [&](auto& server)
        {
            server.set_trusted_proxies({ "127.0.0.1" });
            server.router().template set_http_handler<httplib::method::get>(
                "/limited",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                limiter);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto call = [&](std::string_view ip, httplib::status expected) -> net::awaitable<void>
            {
                auto hdrs = httplib::headers();
                hdrs.set("X-Forwarded-For", std::string(ip));
                httplib::client::request req(httplib::method::get, "/limited", hdrs);
                auto resp = UNWRAP(co_await client.async_send_request(req));
                REQUIRE(resp.result() == expected);
                co_return;
            };

            co_await call("10.0.1.1", httplib::status::ok); // 最久未访问
            co_await call("10.0.1.2", httplib::status::ok); // 最近访问
            co_await call("10.0.1.3", httplib::status::ok); // 挤掉 10.0.1.1

            // 10.0.1.2 的桶没被挤掉，计数延续，因此第 3 次请求仍然被限。
            co_await call("10.0.1.2", httplib::status::ok);
            co_await call("10.0.1.2", httplib::status::too_many_requests);
        });
}

TEST_CASE("Rate Limit: reject policy refuses new clients when the table is full", "[middleware]")
{
    mw::rate_limit_middleware limiter(2, std::chrono::seconds(60));
    limiter.max_tracked_clients(1).when_full(mw::capacity_action::reject);

    run(
        [&](auto& server)
        {
            server.set_trusted_proxies({ "127.0.0.1" });
            server.router().template set_http_handler<httplib::method::get>(
                "/limited",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                limiter);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto call = [&](std::string_view ip, httplib::status expected) -> net::awaitable<void>
            {
                auto hdrs = httplib::headers();
                hdrs.set("X-Forwarded-For", std::string(ip));
                httplib::client::request req(httplib::method::get, "/limited", hdrs);
                auto resp = UNWRAP(co_await client.async_send_request(req));
                REQUIRE(resp.result() == expected);
                co_return;
            };

            co_await call("10.0.2.1", httplib::status::ok);
            REQUIRE(limiter.tracked_clients() == 1);
            co_await call("10.0.2.2", httplib::status::too_many_requests);
            REQUIRE(limiter.tracked_clients() == 1);
        });
}

// ===== Combined middleware =====

TEST_CASE("Combined: cors_middleware + Auth", "[middleware]")
{
    mw::cors_middleware cors;
    mw::basic_auth_middleware auth([](std::string_view u, std::string_view p) { return u == "u" && p == "p"; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/protected",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("protected-data"sv, "text/plain"sv); },
                cors,
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::authorization, "Basic dTpw");

            httplib::client::request req(httplib::method::get, "/protected", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "protected-data");
            REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
            co_return;
        });
}
