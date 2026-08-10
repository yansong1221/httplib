#include "common.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <catch2/catch_test_macros.hpp>
#include <regex>

namespace net = httplib::net;
namespace http = httplib::http;

namespace
{
    using test_common::as_string;
    using test_common::run;
    using test_common::setup_logger;

    void
    set_text(httplib::server::response& resp,
             std::string_view body,
             httplib::http::status status = httplib::http::status::ok)
    {
        resp.set_string_content(body, "text/plain"sv, status);
    }

} // namespace

TEST_CASE("Router: named path parameter :name", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/users/:id",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto id = req.path_param("id");
                    REQUIRE(id == "42");
                    set_text(resp, "user-" + std::string(id));
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/users/42"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "user-42");
            co_return;
        });
}

TEST_CASE("Router: regex path parameter {name:pattern}", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/regex/{id:^\\d+$}",
                [](httplib::server::request& req, httplib::server::response& resp)
                { set_text(resp, "regex-" + std::string(req.path_param("id"))); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/regex/12345"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "regex-12345");
            co_return;
        });
}

TEST_CASE("Router: regex path parameter rejects non-matching input", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/regex/{id:^\\d+$}",
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "should-not-match"); });
            server.router().set_http_not_found_handler(
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "not-found", http::status::not_found); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/regex/abc"));
            REQUIRE(resp.result() == http::status::not_found);
            REQUIRE(as_string(resp) == "not-found");
            co_return;
        });
}

TEST_CASE("Router: wildcard path *", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/files/*",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto rest = req.path_param("*");
                    REQUIRE(rest == "sub/deep/file.txt");
                    set_text(resp, rest);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/files/sub/deep/file.txt"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "sub/deep/file.txt");
            co_return;
        });
}

TEST_CASE("Router: multiple HTTP verbs on one route", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get, http::verb::post>(
                "/multi-verb",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    if (req.method() == http::verb::get)
                    {
                        set_text(resp, "get-response");
                    }
                    else
                    {
                        set_text(resp, "post-response");
                    }
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp_get = UNWRAP(co_await client.async_get("/multi-verb"));
            REQUIRE(resp_get.result() == http::status::ok);
            REQUIRE(as_string(resp_get) == "get-response");

            auto resp_post =
                UNWRAP(co_await client.async_post("/multi-verb", std::string_view("")));
            REQUIRE(resp_post.result() == http::status::ok);
            REQUIRE(as_string(resp_post) == "post-response");
            co_return;
        });
}

TEST_CASE("Router: multiple named parameters", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/blog/:year/:month/:slug",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto y = req.path_param("year");
                    auto m = req.path_param("month");
                    auto s = req.path_param("slug");
                    REQUIRE(y == "2024");
                    REQUIRE(m == "12");
                    REQUIRE(s == "hello-world");
                    set_text(resp, "ok");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp =
                UNWRAP(co_await client.async_get("/blog/2024/12/hello-world"));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });
}

TEST_CASE("Router: path_param template overload", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/user/:id/order/:price",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    int id = req.path_param<int>("id");
                    double price = req.path_param<double>("price");
                    auto name = req.path_param<std::string>("id");
                    REQUIRE(id == 42);
                    REQUIRE(price == 19.99);
                    REQUIRE(name == "42");
                    resp.set_string_content("ok"sv, "text/plain"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp =
                UNWRAP(co_await client.async_get("/user/42/order/19.99"));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });
}

TEST_CASE("Router: set_post_routing_handler for CORS", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::options>(
                "/*",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set("Access-Control-Allow-Origin", "*");
                    resp.set("Access-Control-Allow-Methods", "GET, POST");
                    resp.set_empty_content(http::status::no_content);
                });
            server.router().set_post_routing_handler(
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set("Access-Control-Allow-Origin", "*");
                    resp.set("Access-Control-Allow-Methods", "GET, POST");
                });
            server.router().template set_http_handler<http::verb::post>(
                "/api/data",
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "data-ok"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp =
                UNWRAP(co_await client.async_post("/api/data", std::string_view("{}")));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
            REQUIRE(as_string(resp) == "data-ok");
            co_return;
        });
}

struct test_handler
{
    std::string prefix;

    void
    handle(httplib::server::request& req, httplib::server::response& resp)
    {
        set_text(resp, prefix + "-" + std::string(req.path_param("name")));
    }
};

TEST_CASE("Router: member function handler", "[router]")
{
    test_handler th { "member" };
    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/member/:name", &test_handler::handle, th);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/member/test"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "member-test");
            co_return;
        });
}

TEST_CASE("Router: 405 Method Not Allowed", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/readonly",
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "get-only"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp =
                UNWRAP(co_await client.async_post("/readonly", std::string_view("")));
            REQUIRE(resp.result() == http::status::method_not_allowed);
            REQUIRE(resp["Allow"] == "GET");
            co_return;
        });
}

TEST_CASE("Router: static path priority over param", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/users/all",
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "all-users"); });
            server.router().template set_http_handler<http::verb::get>(
                "/users/:id",
                [](httplib::server::request& req, httplib::server::response& resp)
                { set_text(resp, "user-" + std::string(req.path_param("id"))); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/users/all"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "all-users");

            auto resp2 = UNWRAP(co_await client.async_get("/users/42"));
            REQUIRE(resp2.result() == http::status::ok);
            REQUIRE(as_string(resp2) == "user-42");
            co_return;
        });
}

TEST_CASE("Router: regex param error in pre_routing", "[router]")
{
    net::thread_pool pool{ 1 };
    httplib::server::http_server server(pool.get_executor());
    REQUIRE_THROWS_AS(
        server.router().set_http_handler<http::verb::get>("/bad/{id:[}",
                                                           [](httplib::server::request&,
                                                              httplib::server::response&) {}),
        std::regex_error);
    pool.join();
}

TEST_CASE("Router: trailing slash exact match", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/page/",
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "slash"); });
            server.router().template set_http_handler<http::verb::get>(
                "/page",
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "no-slash"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto r1 = UNWRAP(co_await client.async_get("/page/"));
            REQUIRE(as_string(r1) == "slash");

            auto r2 = UNWRAP(co_await client.async_get("/page"));
            REQUIRE(as_string(r2) == "no-slash");
            co_return;
        });
}

TEST_CASE("Router: returns 404 for missing route", "[router]")
{
    run(
        [](auto&) {},
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/nonexistent"));
            REQUIRE(resp.result() == http::status::not_found);
            co_return;
        });
}
