#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/html/cookie.hpp"
#include "httplib/server/middleware/session.hpp"
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
namespace http = httplib::http;
namespace net  = httplib::net;
namespace mw   = httplib::server::middleware;

namespace {

struct test_scaffold
{
    net::io_context ioc;
    httplib::server::http_server server;
    httplib::tcp::endpoint endpoint;
    std::thread thread;
    std::unique_ptr<httplib::client::http_client> client;

    test_scaffold()
        : server(ioc)
    {
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        server.set_logger(std::make_shared<spdlog::logger>("httplib.tests", null_sink));
    }

    ~test_scaffold()
    {
        server.async_stop().wait();
        ioc.stop();
        if (thread.joinable())
            thread.join();
    }

    void start()
    {
        server.listen("127.0.0.1", 0);
        endpoint = server.local_endpoint();
        server.async_run();
        thread = std::thread([this] { ioc.run(); });

        client = std::make_unique<httplib::client::http_client>(
            ioc.get_executor(), endpoint.address().to_string(), endpoint.port());
        client->set_timeout(std::chrono::seconds(5));
    }

    auto& router() { return server.router(); }
};

std::string as_string(const httplib::client::http_client::response& resp)
{
    return resp.body().as<httplib::body::string_body>();
}

#define UNWRAP(result)                                                                             \
    [&](auto&& r) {                                                                                \
        REQUIRE(r.has_value());                                                                    \
        return std::move(r).value();                                                               \
    }(result)

} // namespace

// ===== cookie_jar tests =====

TEST_CASE("cookie_jar: parse simple cookies", "[cookie]")
{
    auto jar = httplib::html::cookie_jar::parse("name=value");

    REQUIRE(jar.size() == 1);
    REQUIRE(jar.has("name"));
    REQUIRE(jar.get("name").value() == "value");
}

TEST_CASE("cookie_jar: parse multiple cookies", "[cookie]")
{
    auto jar = httplib::html::cookie_jar::parse("a=1; b=2; c=3");

    REQUIRE(jar.size() == 3);
    REQUIRE(jar.get("a").value() == "1");
    REQUIRE(jar.get("b").value() == "2");
    REQUIRE(jar.get("c").value() == "3");
}

TEST_CASE("cookie_jar: parse with spaces", "[cookie]")
{
    auto jar = httplib::html::cookie_jar::parse(" key = val ");

    REQUIRE(jar.size() == 1);
    REQUIRE(jar.get("key").value() == "val");
}

TEST_CASE("cookie_jar: parse empty", "[cookie]")
{
    auto jar = httplib::html::cookie_jar::parse("");

    REQUIRE(jar.size() == 0);
    REQUIRE_FALSE(jar.has("anything"));
}

TEST_CASE("cookie_jar: to_set_cookie_string minimal", "[cookie]")
{
    httplib::html::cookie ck;
    ck.name  = "sid";
    ck.value = "abc123";
    ck.path  = "/";

    auto s = ck.to_set_cookie_string();
    REQUIRE(s.starts_with("sid=abc123"));
    REQUIRE(s.find("Path=/") != std::string::npos);
    REQUIRE(s.find("HttpOnly") != std::string::npos);
    REQUIRE(s.find("SameSite=Lax") != std::string::npos);
}

TEST_CASE("cookie_jar: to_set_cookie_string with options", "[cookie]")
{
    httplib::html::cookie ck;
    ck.name      = "token";
    ck.value     = "xyz";
    ck.path      = "/api";
    ck.max_age   = std::chrono::seconds(3600);
    ck.secure    = true;
    ck.http_only = true;
    ck.same_site = httplib::html::cookie::same_site_t::strict;

    auto s = ck.to_set_cookie_string();
    REQUIRE(s.find("Max-Age=3600") != std::string::npos);
    REQUIRE(s.find("Secure") != std::string::npos);
    REQUIRE(s.find("SameSite=Strict") != std::string::npos);
}

// ===== session middleware tests =====

TEST_CASE("Session: middleware creates new session ID", "[session]")
{
    mw::session_middleware sm;

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/visit",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto sess = req.session();
            REQUIRE(sess);
            REQUIRE_FALSE(sess->id().empty());
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        sm);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/visit"));
    REQUIRE(resp.result() == http::status::ok);

    auto set_cookie = std::string(resp[http::field::set_cookie]);
    REQUIRE_FALSE(set_cookie.empty());
    REQUIRE(set_cookie.starts_with("session_id="));
}

TEST_CASE("Session: middleware persists data across requests", "[session]")
{
    mw::session_middleware sm;

    test_scaffold ts;

    ts.router().set_http_handler<http::verb::post>(
        "/login",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto sess = req.session();
            sess->set("user", "alice");
            resp.set_string_content("logged-in"sv, "text/plain"sv);
        },
        sm);

    ts.router().set_http_handler<http::verb::get>(
        "/whoami",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto sess = req.session();
            auto user = sess->get("user");
            resp.set_string_content(user.value_or("anonymous"), "text/plain"sv);
        },
        sm);

    ts.start();

    // First request: login, get session cookie
    auto resp1 = UNWRAP(ts.client->post("/login", ""sv));
    REQUIRE(resp1.result() == http::status::ok);
    auto cookie = std::string(resp1[http::field::set_cookie]);

    // Second request: use session cookie
    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::cookie, cookie);

    auto resp2 =
        UNWRAP(ts.client->send_request(http::verb::get, "/whoami", std::string_view {}, hdrs));
    REQUIRE(resp2.result() == http::status::ok);
    REQUIRE(as_string(resp2) == "alice");
}

TEST_CASE("Session: get_session returns valid pointer", "[session]")
{
    mw::session_middleware sm;

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/data",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto sess = req.session();
            REQUIRE(sess);
            sess->set("count", "1");
            auto c = sess->get("count");
            REQUIRE(c.has_value());
            REQUIRE(*c == "1");
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        sm);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/data"));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Session: session has and remove", "[session]")
{
    mw::session_middleware sm;

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/ops",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto sess = req.session();
            sess->set("temp", "data");
            REQUIRE(sess->has("temp"));
            REQUIRE_FALSE(sess->empty());
            sess->remove("temp");
            REQUIRE_FALSE(sess->has("temp"));
            REQUIRE(sess->empty());
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        sm);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/ops"));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Session: custom store can be injected", "[session]")
{
    auto store = std::make_shared<mw::memory_session_store>(std::chrono::seconds(60));
    mw::session_middleware sm(store);

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/custom",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto sess = req.session();
            sess->set("store", "injected");
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        sm);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/custom"));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Session: configurable cookie name", "[session]")
{
    mw::session_config cfg;
    cfg.cookie_name = "my_session";
    cfg.cookie_path = "/app";
    mw::session_middleware sm(cfg);

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/named",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto sess = req.session();
            sess->set("key", "val");
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        sm);
    ts.start();

    auto resp       = UNWRAP(ts.client->get("/named"));
    auto set_cookie = std::string(resp[http::field::set_cookie]);
    REQUIRE(set_cookie.starts_with("my_session="));
    REQUIRE(set_cookie.find("Path=/app") != std::string::npos);
}
