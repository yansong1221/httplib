#include "common.hpp"
#include "httplib/server/middleware/data.hpp"
#include "httplib/server/middleware/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "server/middleware/memory_store.hpp"
#include <chrono>

namespace mw = httplib::server::middleware;

namespace
{
    using test_common::as_string;
    using test_common::test_scaffold;

} // namespace

// ===== session middleware tests =====

TEST_CASE("Session: middleware creates new session ID", "[session]")
{
    mw::session_middleware sm;

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/visit",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            auto sess = mw::get_data<mw::session_middleware>(req);
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
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            auto sess = mw::get_data<mw::session_middleware>(req);
            sess->set("user", "alice");
            resp.set_string_content("logged-in"sv, "text/plain"sv);
        },
        sm);

    ts.router().set_http_handler<http::verb::get>(
        "/whoami",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            auto sess = mw::get_data<mw::session_middleware>(req);
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

    auto resp2 = UNWRAP(ts.client->get("/whoami", {}, hdrs));
    REQUIRE(resp2.result() == http::status::ok);
    REQUIRE(as_string(resp2) == "alice");
}

TEST_CASE("Session: get_session returns valid pointer", "[session]")
{
    mw::session_middleware sm;

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/data",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            auto sess = mw::get_data<mw::session_middleware>(req);
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
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            auto sess = mw::get_data<mw::session_middleware>(req);
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
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            auto sess = mw::get_data<mw::session_middleware>(req);
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
    mw::session_middleware sm;
    sm.cookie_name("my_session").cookie_path("/app");

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/named",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            auto sess = mw::get_data<mw::session_middleware>(req);
            sess->set("key", "val");
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        sm);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/named"));
    auto set_cookie = std::string(resp[http::field::set_cookie]);
    REQUIRE(set_cookie.starts_with("my_session="));
    REQUIRE(set_cookie.find("Path=/app") != std::string::npos);
}

TEST_CASE("Session: cookie attributes http_only, secure, max_age", "[session]")
{
    mw::session_middleware sm;
    sm.cookie_name("attrs").http_only(true).secure(true).max_age(std::chrono::hours(1));

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/attrs",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            mw::get_data<mw::session_middleware>(req)->set("x", "1");
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        sm);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/attrs"));
    auto set_cookie = std::string(resp[http::field::set_cookie]);
    REQUIRE(set_cookie.find("HttpOnly") != std::string::npos);
    REQUIRE(set_cookie.find("Secure") != std::string::npos);
    REQUIRE(set_cookie.find("Max-Age=3600") != std::string::npos);
}

TEST_CASE("Session: same_site strict", "[session]")
{
    mw::session_middleware sm;
    sm.cookie_name("samesite").same_site_strict();

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/samesite",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            mw::get_data<mw::session_middleware>(req)->set("x", "1");
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        sm);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/samesite"));
    auto set_cookie = std::string(resp[http::field::set_cookie]);
    REQUIRE(set_cookie.find("SameSite=Strict") != std::string::npos);
}

TEST_CASE("Session: max_age cookie attribute", "[session]")
{
    mw::session_middleware sm;
    sm.cookie_name("aged").max_age(std::chrono::seconds(1800));

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/aged",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            mw::get_data<mw::session_middleware>(req)->set("x", "1");
            resp.set_string_content("ok"sv, "text/plain"sv);
        },
        sm);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/aged"));
    auto set_cookie = std::string(resp[http::field::set_cookie]);
    REQUIRE(set_cookie.find("Max-Age=1800") != std::string::npos);
}
