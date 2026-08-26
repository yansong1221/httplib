#include "common.hpp"
#include "httplib/body/string_body.hpp"
#include "httplib/server/middleware/auth.hpp"
#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/middleware/rate_limit.hpp"
#include "httplib/server/response.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>

namespace body = httplib::body;
namespace mw = httplib::server::middleware;
namespace net = httplib::net;
namespace http = httplib::http;
using test_common::run;
using test_common::setup_logger;

namespace
{

    template <typename Setup, typename Test>
    void
    run_with_ep(Setup&& setup, Test&& test)
    {
        net::thread_pool pool{ 1 };
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

                httplib::client::http_client client(pool.get_executor(),
                                                     ep.address().to_string(),
                                                     ep.port());
                client.set_timeout(std::chrono::seconds(5));

                co_await test(client, ep, pool);

                client.close();
                server.stop();
            },
            [&](std::exception_ptr e)
            {
                err = e;
            });

        pool.join();
        if (err)
            std::rethrow_exception(err);
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
            server.router().template set_http_handler<http::verb::get>(
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
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(*order
                    == std::vector<std::string>{ "global_before", "route_before", "handler",
                                                 "route_after", "global_after" });
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

            server.router().template set_http_handler<http::verb::get>(
                "/public",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); });
            server.router().template set_http_handler<http::verb::get>(
                "/private",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret"sv, "text/plain"sv); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::authorization, "Basic YWRtaW46c2VjcmV0");
            auto resp1 = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::get, "/public", hdrs)));
            REQUIRE(resp1.result() == http::status::ok);

            auto resp2 = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::get, "/private", hdrs)));
            REQUIRE(resp2.result() == http::status::ok);

            auto resp3 = UNWRAP(co_await client.async_get("/public"));
            REQUIRE(resp3.result() == http::status::unauthorized);
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

            server.router().template set_http_handler<http::verb::get>(
                "/gc",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/gc"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(std::string(resp[http::field::access_control_allow_origin]) == "x");
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
            server.router().template set_http_handler<http::verb::get>(
                "/data",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                cors);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/data"));
            REQUIRE(resp.result() == http::status::ok);
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
            server.router().template set_http_handler<http::verb::options>(
                "/data",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("should-not-reach"sv, "text/plain"sv); },
                cors);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_options("/data"));
            REQUIRE(resp.result() == http::status::no_content);
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
            server.router().template set_http_handler<http::verb::get>(
                "/data",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                std::move(cors));
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/data"));
            REQUIRE(resp.result() == http::status::ok);
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
            server.router().template set_http_handler<http::verb::get>(
                "/cors_middleware-multi",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("cors_middleware-data"sv, "text/plain"sv); },
                cors);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::origin, "https://a.com");
            auto resp = UNWRAP(
                co_await client.async_send_request(httplib::client::request(http::verb::get, "/cors_middleware-multi", hdrs)));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "cors_middleware-data");
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
            server.router().template set_http_handler<http::verb::options>(
                "/cors_middleware-methods",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_empty_content(http::status::no_content); },
                cors);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::origin, "https://x.com");
            auto resp = UNWRAP(
                co_await client.async_send_request(httplib::client::request(http::verb::options, "/cors_middleware-methods", hdrs)));
            REQUIRE(resp.result() == http::status::no_content);
            auto methods = std::string(resp["Access-Control-Allow-Methods"]);
            REQUIRE(methods.find("PUT") != std::string::npos);
            REQUIRE(methods.find("PATCH") != std::string::npos);
            co_return;
        });
}

// ===== Basic Auth =====

TEST_CASE("Basic Auth: valid credentials pass through", "[middleware]")
{
    mw::basic_auth_middleware auth(
        [](std::string_view u, std::string_view p) { return u == "user" && p == "pass"; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/secret",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret-data"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::authorization, "Basic dXNlcjpwYXNz");

            auto resp = UNWRAP(
                co_await client.async_send_request(httplib::client::request(http::verb::get, "/secret", hdrs)));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "secret-data");
            co_return;
        });
}

TEST_CASE("Basic Auth: invalid credentials return 401", "[middleware]")
{
    mw::basic_auth_middleware auth(
        [](std::string_view u, std::string_view p) { return u == "user" && p == "pass"; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/secret",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret-data"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::authorization, "Basic dXNlcjp3cm9uZw==");

            auto resp = UNWRAP(
                co_await client.async_send_request(httplib::client::request(http::verb::get, "/secret", hdrs)));
            REQUIRE(resp.result() == http::status::unauthorized);
            co_return;
        });
}

TEST_CASE("Basic Auth: missing header returns 401", "[middleware]")
{
    mw::basic_auth_middleware auth([](std::string_view, std::string_view) { return true; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/secret",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret-data"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/secret"));
            REQUIRE(resp.result() == http::status::unauthorized);
            REQUIRE_FALSE(std::string(resp[http::field::www_authenticate]).empty());
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
            server.router().template set_http_handler<http::verb::get>(
                "/token-area",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::authorization, "Bearer abc-123");

            auto resp = UNWRAP(
                co_await client.async_send_request(httplib::client::request(http::verb::get, "/token-area", hdrs)));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });
}

TEST_CASE("Bearer Auth: invalid token returns 401", "[middleware]")
{
    mw::bearer_auth_middleware auth([](std::string_view t) { return t == "abc-123"; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/token-area",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::authorization, "Bearer wrong-token");

            auto resp = UNWRAP(
                co_await client.async_send_request(httplib::client::request(http::verb::get, "/token-area", hdrs)));
            REQUIRE(resp.result() == http::status::unauthorized);
            co_return;
        });
}

TEST_CASE("Bearer Auth: missing header returns 401", "[middleware]")
{
    mw::bearer_auth_middleware auth([](std::string_view) { return true; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/bearer-missing",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/bearer-missing"));
            REQUIRE(resp.result() == http::status::unauthorized);
            co_return;
        });
}

TEST_CASE("Bearer Auth: non-Bearer scheme returns 401", "[middleware]")
{
    mw::bearer_auth_middleware auth([](std::string_view) { return true; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/bearer-scheme",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("secret"sv, "text/plain"sv); },
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::authorization, "Digest xxx");
            auto resp = UNWRAP(
                co_await client.async_send_request(httplib::client::request(http::verb::get, "/bearer-scheme", hdrs)));
            REQUIRE(resp.result() == http::status::unauthorized);
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
            server.router().template set_http_handler<http::verb::get>(
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
                REQUIRE(resp.result() == http::status::ok);
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
            server.router().template set_http_handler<http::verb::get>(
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
                REQUIRE(resp.result() == http::status::ok);
            }

            auto resp = UNWRAP(co_await client.async_get("/limited"));
            REQUIRE(resp.result() == http::status::too_many_requests);
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
            server.router().template set_http_handler<http::verb::get>(
                "/a",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("a"sv, "text/plain"sv); },
                limiter);

            server.router().template set_http_handler<http::verb::get>(
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
            REQUIRE(resp.result() == http::status::too_many_requests);
            co_return;
        });
}

TEST_CASE("Rate Limit: shared limits apply across routes", "[middleware]")
{
    auto limiter = std::make_shared<mw::rate_limit_middleware>(2, std::chrono::seconds(60));

    run_with_ep(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
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
            REQUIRE(blocked->result() == http::status::too_many_requests);

            auto client2 = std::make_unique<httplib::client::http_client>(
                pool.get_executor(),
                ep.address().to_string(),
                ep.port());
            client2->set_timeout(std::chrono::seconds(5));
            auto resp2 = co_await client2->async_get("/rl-ip");
            REQUIRE(resp2.has_value());
            REQUIRE(resp2->result() == http::status::too_many_requests);
            client2->close();
            co_return;
        });
}

// ===== Combined middleware =====

TEST_CASE("Combined: cors_middleware + Auth", "[middleware]")
{
    mw::cors_middleware cors;
    mw::basic_auth_middleware auth(
        [](std::string_view u, std::string_view p) { return u == "u" && p == "p"; });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/protected",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("protected-data"sv, "text/plain"sv); },
                cors,
                auth);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::authorization, "Basic dTpw");

            auto resp = UNWRAP(
                co_await client.async_send_request(httplib::client::request(http::verb::get, "/protected", hdrs)));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "protected-data");
            REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
            co_return;
        });
}
