#include "common.hpp"
#include "httplib/server/middleware/auth.hpp"
#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/middleware/rate_limit.hpp"
#include "httplib/server/response.hpp"
#include <chrono>

namespace mw = httplib::server::middleware;

namespace
{
    using test_common::as_string;
    using test_common::test_scaffold;

    TEST_CASE("Global middleware: execution order with route middleware", "[middleware]")
    {
        test_scaffold ts;
        auto order = std::make_shared<std::vector<std::string>>();

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

        ts.router().use(order_mw { "global", order });

        ts.router().set_http_handler<http::verb::get>(
            "/order",
            [order](httplib::server::request&, httplib::server::response& resp)
            {
                order->push_back("handler");
                resp.set_string_content("ok"sv, "text/plain"sv);
            },
            order_mw { "route", order });
        ts.start();

        auto resp = UNWRAP(ts.client->get("/order"));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(
            *order
            == std::vector<std::string> { "global_before", "route_before", "handler", "route_after", "global_after" });
    }

} // namespace

// ===== CORS =====

TEST_CASE("CORS: allows request without Origin", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/data",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("ok"sv, "text/plain"sv); },
        mw::cors {});
    ts.start();

    auto resp = UNWRAP(ts.client->get("/data"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
    REQUIRE_FALSE(resp["Access-Control-Allow-Methods"].empty());
}

TEST_CASE("CORS: OPTIONS preflight is short-circuited", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::options>(
        "/data",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("should-not-reach"sv, "text/plain"sv); },
        mw::cors {});
    ts.start();

    auto resp = UNWRAP(ts.client->options("/data"));
    REQUIRE(resp.result() == http::status::no_content);
    REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
}

TEST_CASE("CORS: custom origin and credentials", "[middleware]")
{
    test_scaffold ts;
    auto cors = mw::cors().allow_origin("https://example.com").allow_credentials(true).max_age(3600);

    ts.router().set_http_handler<http::verb::get>(
        "/data",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("ok"sv, "text/plain"sv); },
        std::move(cors));
    ts.start();

    auto resp = UNWRAP(ts.client->get("/data"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp["Access-Control-Allow-Origin"] == "https://example.com");
    REQUIRE(resp["Access-Control-Allow-Credentials"] == "true");
    REQUIRE(resp["Access-Control-Max-Age"] == "3600");
}

// ===== Basic Auth =====

TEST_CASE("Basic Auth: valid credentials pass through", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/secret",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("secret-data"sv, "text/plain"sv); },
        mw::basic_auth([](std::string_view u, std::string_view p) { return u == "user" && p == "pass"; }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Basic dXNlcjpwYXNz");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/secret", hdrs));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "secret-data");
}

TEST_CASE("Basic Auth: invalid credentials return 401", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/secret",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("secret-data"sv, "text/plain"sv); },
        mw::basic_auth([](std::string_view u, std::string_view p) { return u == "user" && p == "pass"; }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Basic dXNlcjp3cm9uZw==");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/secret", hdrs));
    REQUIRE(resp.result() == http::status::unauthorized);
}

TEST_CASE("Basic Auth: missing header returns 401", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/secret",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("secret-data"sv, "text/plain"sv); },
        mw::basic_auth([](std::string_view, std::string_view) { return true; }));
    ts.start();

    auto resp = UNWRAP(ts.client->get("/secret"));
    REQUIRE(resp.result() == http::status::unauthorized);
    REQUIRE_FALSE(std::string(resp[http::field::www_authenticate]).empty());
}

// ===== Bearer Auth =====

TEST_CASE("Bearer Auth: valid token passes through", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/token-area",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("ok"sv, "text/plain"sv); },
        mw::bearer_auth([](std::string_view t) { return t == "abc-123"; }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Bearer abc-123");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/token-area", hdrs));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Bearer Auth: invalid token returns 401", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/token-area",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("ok"sv, "text/plain"sv); },
        mw::bearer_auth([](std::string_view t) { return t == "abc-123"; }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Bearer wrong-token");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/token-area", hdrs));
    REQUIRE(resp.result() == http::status::unauthorized);
}

// ===== Rate Limit =====

TEST_CASE("Rate Limit: allows requests within limit", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/limited",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("ok"sv, "text/plain"sv); },
        mw::rate_limit(10, std::chrono::seconds(60)));
    ts.start();

    for (int i = 0; i < 5; ++i)
    {
        auto resp = UNWRAP(ts.client->get("/limited"));
        REQUIRE(resp.result() == http::status::ok);
    }
}

TEST_CASE("Rate Limit: blocks after exceeding limit", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/limited",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("ok"sv, "text/plain"sv); },
        mw::rate_limit(3, std::chrono::seconds(60)));
    ts.start();

    for (int i = 0; i < 3; ++i)
    {
        auto resp = UNWRAP(ts.client->get("/limited"));
        REQUIRE(resp.result() == http::status::ok);
    }

    auto resp = UNWRAP(ts.client->get("/limited"));
    REQUIRE(resp.result() == http::status::too_many_requests);
    REQUIRE_FALSE(std::string(resp["Retry-After"]).empty());
}

TEST_CASE("Rate Limit: shared instance across routes", "[middleware]")
{
    auto limiter = mw::rate_limit(2, std::chrono::seconds(60));

    test_scaffold ts;

    ts.router().set_http_handler<http::verb::get>(
        "/a",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("a"sv, "text/plain"sv); },
        limiter);

    ts.router().set_http_handler<http::verb::get>(
        "/b",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("b"sv, "text/plain"sv); },
        limiter);

    ts.start();

    UNWRAP(ts.client->get("/a"));
    UNWRAP(ts.client->get("/b"));

    auto resp = UNWRAP(ts.client->get("/a"));
    REQUIRE(resp.result() == http::status::too_many_requests);
}

// ===== Combined middleware =====

TEST_CASE("Combined: CORS + Auth", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/protected",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("protected-data"sv, "text/plain"sv); },
        mw::cors {},
        mw::basic_auth([](std::string_view u, std::string_view p) { return u == "u" && p == "p"; }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Basic dTpw");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/protected", hdrs));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "protected-data");
    REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
}

TEST_CASE("CORS: allow_origins with multiple origins", "[middleware]")
{
    test_scaffold ts;
    mw::cors cors;
    cors.allow_origins({ "https://a.com", "https://b.com" });

    ts.router().set_http_handler<http::verb::get>(
        "/cors-multi",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("cors-data"sv, "text/plain"sv); },
        cors);
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::origin, "https://a.com");
    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/cors-multi", hdrs));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "cors-data");
}

TEST_CASE("CORS: allow_methods custom", "[middleware]")
{
    test_scaffold ts;
    mw::cors cors;
    cors.allow_methods({ "PUT", "PATCH" });
    cors.allow_origin("https://x.com");

    ts.router().set_http_handler<http::verb::options>(
        "/cors-methods",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_empty_content(http::status::no_content); },
        cors);
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::origin, "https://x.com");
    auto resp = UNWRAP(ts.client->send_request(http::verb::options, "/cors-methods", hdrs));
    REQUIRE(resp.result() == http::status::no_content);
    auto methods = std::string(resp["Access-Control-Allow-Methods"]);
    REQUIRE(methods.find("PUT") != std::string::npos);
    REQUIRE(methods.find("PATCH") != std::string::npos);
}

TEST_CASE("Rate Limit: shared limits apply across routes", "[middleware]")
{
    test_scaffold ts;
    auto limiter = std::make_shared<mw::rate_limit>(2, std::chrono::seconds(60));

    ts.router().set_http_handler<http::verb::get>(
        "/rl-ip",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("ok"sv, "text/plain"sv); },
        *limiter);
    ts.start();

    // First client from default IP (127.0.0.1) uses 2 requests
    UNWRAP(ts.client->get("/rl-ip"));
    UNWRAP(ts.client->get("/rl-ip"));
    auto blocked = ts.client->get("/rl-ip");
    REQUIRE(blocked.has_value());
    REQUIRE(blocked->result() == http::status::too_many_requests);

    // Second client from same IP should also be rate-limited (shared bucket)
    auto client2 = std::make_unique<httplib::client::http_client>(ts.executor(),
                                                                  ts.endpoint.address().to_string(),
                                                                  ts.endpoint.port());
    client2->set_timeout(std::chrono::seconds(5));
    auto resp2 = client2->get("/rl-ip");
    REQUIRE(resp2.has_value());
    REQUIRE(resp2->result() == http::status::too_many_requests);
}

TEST_CASE("Bearer Auth: missing header returns 401", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/bearer-missing",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("secret"sv, "text/plain"sv); },
        mw::bearer_auth([](std::string_view) { return true; }));
    ts.start();

    auto resp = UNWRAP(ts.client->get("/bearer-missing"));
    REQUIRE(resp.result() == http::status::unauthorized);
}

TEST_CASE("Bearer Auth: non-Bearer scheme returns 401", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/bearer-scheme",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("secret"sv, "text/plain"sv); },
        mw::bearer_auth([](std::string_view) { return true; }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Digest xxx");
    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/bearer-scheme", hdrs));
    REQUIRE(resp.result() == http::status::unauthorized);
}

TEST_CASE("Global middleware: applies to all routes", "[middleware]")
{
    test_scaffold ts;

    ts.router().use(
        mw::basic_auth([](std::string_view u, std::string_view p) { return u == "admin" && p == "secret"; }));

    ts.router().set_http_handler<http::verb::get>("/public",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_string_content("ok"sv, "text/plain"sv); });
    ts.router().set_http_handler<http::verb::get>("/private",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_string_content("secret"sv, "text/plain"sv); });
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Basic YWRtaW46c2VjcmV0");
    auto resp1 = UNWRAP(ts.client->send_request(http::verb::get, "/public", hdrs));
    REQUIRE(resp1.result() == http::status::ok);

    auto resp2 = UNWRAP(ts.client->send_request(http::verb::get, "/private", hdrs));
    REQUIRE(resp2.result() == http::status::ok);

    auto resp3 = UNWRAP(ts.client->get("/public"));
    REQUIRE(resp3.result() == http::status::unauthorized);
}

TEST_CASE("Global middleware: cors via use()", "[middleware]")
{
    test_scaffold ts;

    ts.router().use(mw::cors {}.allow_origin("x"));

    ts.router().set_http_handler<http::verb::get>("/gc",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_string_content("ok"sv, "text/plain"sv); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/gc"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(std::string(resp[http::field::access_control_allow_origin]) == "x");
}
