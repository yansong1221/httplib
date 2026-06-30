#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/server/middleware/auth.hpp"
#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/middleware/rate_limit.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

using namespace std::string_view_literals;
namespace http  = httplib::http;
namespace net   = httplib::net;
namespace mw    = httplib::server::middleware;

namespace {

struct test_scaffold {
    net::io_context ioc;
    httplib::server::http_server server;
    httplib::tcp::endpoint endpoint;
    std::thread thread;
    std::unique_ptr<httplib::client::http_client> client;

    test_scaffold() : server(ioc)
    {
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        server.set_logger(std::make_shared<spdlog::logger>("httplib.tests", null_sink));
    }

    ~test_scaffold()
    {
        server.stop().wait();
        ioc.stop();
        if (thread.joinable())
            thread.join();
    }

    void start()
    {
        server.listen("127.0.0.1", 0);
        endpoint = server.local_endpoint();
        server.run();
        thread = std::thread([this] { ioc.run(); });

        client = std::make_unique<httplib::client::http_client>(
            ioc.get_executor(), endpoint.address().to_string(), endpoint.port());
        client->set_timeout(std::chrono::seconds(5));
    }

    auto& router() { return server.router(); }

    std::string host() const { return endpoint.address().to_string(); }
    uint16_t port() const { return endpoint.port(); }
};

std::string as_string(const httplib::client::http_client::response& resp)
{
    return resp.body().as<httplib::body::string_body>();
}

#define UNWRAP(result)              \
    [&](auto&& r) {                 \
        REQUIRE(r.has_value());     \
        return std::move(r).value(); \
    }(result)

} // namespace

// ===== CORS =====

TEST_CASE("CORS: allows request without Origin", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/data",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        mw::cors_middleware{});
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
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("should-not-reach"sv, "text/plain"sv);
        },
        mw::cors_middleware{});
    ts.start();

    auto resp = UNWRAP(ts.client->options("/data"));
    REQUIRE(resp.result() == http::status::no_content);
    REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
}

TEST_CASE("CORS: custom origin and credentials", "[middleware]")
{
    test_scaffold ts;
    auto cors = mw::cors_middleware()
                    .allow_origin("https://example.com")
                    .allow_credentials(true)
                    .max_age(3600);

    ts.router().set_http_handler<http::verb::get>(
        "/data",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
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
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("secret-data"sv, "text/plain"sv);
        },
        mw::basic_auth_middleware([](std::string_view u, std::string_view p) {
            return u == "user" && p == "pass";
        }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Basic dXNlcjpwYXNz");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/secret",
                                                std::string_view{}, hdrs));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "secret-data");
}

TEST_CASE("Basic Auth: invalid credentials return 401", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/secret",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("secret-data"sv, "text/plain"sv);
        },
        mw::basic_auth_middleware([](std::string_view u, std::string_view p) {
            return u == "user" && p == "pass";
        }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Basic dXNlcjp3cm9uZw==");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/secret",
                                                std::string_view{}, hdrs));
    REQUIRE(resp.result() == http::status::unauthorized);
}

TEST_CASE("Basic Auth: missing header returns 401", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/secret",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("secret-data"sv, "text/plain"sv);
        },
        mw::basic_auth_middleware([](std::string_view, std::string_view) {
            return true;
        }));
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
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        mw::bearer_auth_middleware([](std::string_view t) {
            return t == "abc-123";
        }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Bearer abc-123");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/token-area",
                                                std::string_view{}, hdrs));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Bearer Auth: invalid token returns 401", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/token-area",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        mw::bearer_auth_middleware([](std::string_view t) {
            return t == "abc-123";
        }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Bearer wrong-token");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/token-area",
                                                std::string_view{}, hdrs));
    REQUIRE(resp.result() == http::status::unauthorized);
}

// ===== Rate Limit =====

TEST_CASE("Rate Limit: allows requests within limit", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/limited",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        mw::rate_limit_middleware(10, std::chrono::seconds(60)));
    ts.start();

    for (int i = 0; i < 5; ++i) {
        auto resp = UNWRAP(ts.client->get("/limited"));
        REQUIRE(resp.result() == http::status::ok);
    }
}

TEST_CASE("Rate Limit: blocks after exceeding limit", "[middleware]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/limited",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        mw::rate_limit_middleware(3, std::chrono::seconds(60)));
    ts.start();

    for (int i = 0; i < 3; ++i) {
        auto resp = UNWRAP(ts.client->get("/limited"));
        REQUIRE(resp.result() == http::status::ok);
    }

    auto resp = UNWRAP(ts.client->get("/limited"));
    REQUIRE(resp.result() == http::status::too_many_requests);
    REQUIRE_FALSE(std::string(resp["Retry-After"]).empty());
}

TEST_CASE("Rate Limit: shared instance across routes", "[middleware]")
{
    auto limiter = mw::rate_limit_middleware(2, std::chrono::seconds(60));

    test_scaffold ts;

    ts.router().set_http_handler<http::verb::get>(
        "/a", [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("a"sv, "text/plain"sv);
        }, limiter);

    ts.router().set_http_handler<http::verb::get>(
        "/b", [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("b"sv, "text/plain"sv);
        }, limiter);

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
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("protected-data"sv, "text/plain"sv);
        },
        mw::cors_middleware{},
        mw::basic_auth_middleware([](std::string_view u, std::string_view p) {
            return u == "u" && p == "p";
        }));
    ts.start();

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::authorization, "Basic dTpw");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/protected",
                                                std::string_view{}, hdrs));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "protected-data");
    REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
}
