#include "common.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/beast/http/status.hpp>
#include <catch2/catch_test_macros.hpp>

namespace net = httplib::net;

namespace
{
    using test_common::as_string;
    using test_common::run;
    using test_common::setup_logger;

    void
    set_text(httplib::server::response& resp, std::string_view body, http::status status = http::status::ok)
    {
        resp.set_string_content(body, "text/plain"sv, status);
    }

} // namespace

struct logging_aspect
{
    std::string& log;

    bool
    before(httplib::server::request&, httplib::server::response&)
    {
        log += "before;";
        return true;
    }

    bool
    after(httplib::server::request&, httplib::server::response&)
    {
        log += "after;";
        return true;
    }
};

TEST_CASE("Aspect: before and after are called", "[aspect]")
{
    std::string log;

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/aspect",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "handler"); },
                logging_aspect { log });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/aspect"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "handler");
            REQUIRE(log == "before;after;");
        });
}

TEST_CASE("Aspect: before returning false stops handler chain", "[aspect]")
{
    struct blocking_aspect
    {
        bool
        before(httplib::server::request&, httplib::server::response& resp)
        {
            resp.set_string_content("blocked"sv, "text/plain"sv, http::status::forbidden);
            return false;
        }
        bool
        after(httplib::server::request&, httplib::server::response&)
        {
            return true;
        }
    };

    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/blocked",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "should-not-reach"); },
                blocking_aspect {});
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/blocked"));
            REQUIRE(resp.result() == http::status::forbidden);
            REQUIRE(as_string(resp) == "blocked");
        });
}

TEST_CASE("Aspect: multiple aspects chain in order", "[aspect]")
{
    std::string order;

    struct aspect_a
    {
        std::string& o;
        bool
        before(httplib::server::request&, httplib::server::response&)
        {
            o += "A.before;";
            return true;
        }
        bool
        after(httplib::server::request&, httplib::server::response&)
        {
            o += "A.after;";
            return true;
        }
    };

    struct aspect_b
    {
        std::string& o;
        bool
        before(httplib::server::request&, httplib::server::response&)
        {
            o += "B.before;";
            return true;
        }
        bool
        after(httplib::server::request&, httplib::server::response&)
        {
            o += "B.after;";
            return true;
        }
    };

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/multi-aspect",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "ok"); },
                aspect_a { order },
                aspect_b { order });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/multi-aspect"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(order == "A.before;B.before;A.after;B.after;");
        });
}

TEST_CASE("Aspect: 404 handler also supports aspects", "[aspect]")
{
    std::string log;

    run(
        [&](auto& server)
        {
            server.router().set_http_not_found_handler(
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "custom-404", http::status::not_found); },
                logging_aspect { log });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/non-existent"));
            REQUIRE(resp.result() == http::status::not_found);
            REQUIRE(as_string(resp) == "custom-404");
            REQUIRE(log == "before;after;");
        });
}
