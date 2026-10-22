#include "common.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <catch2/catch_test_macros.hpp>
#include <regex>
#include <stdexcept>
#include <string>

namespace net = httplib::net;

namespace
{
    using test_common::as_string;
    using test_common::run;
    using test_common::setup_logger;

    void
    set_text(httplib::server::response& resp,
             std::string_view body,
             httplib::status st = httplib::status::ok)
    {
        resp.set_string_content(body, "text/plain"sv, st);
    }

} // namespace

TEST_CASE("Router: named path parameter :name", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
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
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(as_string(resp) == "user-42");
            co_return;
        });
}

TEST_CASE("Router: regex path parameter {name:pattern}", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/regex/{id:^\\d+$}",
                [](httplib::server::request& req, httplib::server::response& resp)
                { set_text(resp, "regex-" + std::string(req.path_param("id"))); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/regex/12345"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(as_string(resp) == "regex-12345");
            co_return;
        });
}

TEST_CASE("Router: regex path parameter rejects non-matching input", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/regex/{id:^\\d+$}",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "should-not-match"); });
            server.router().set_http_not_found_handler([](httplib::server::request&, httplib::server::response& resp)
                                                       { set_text(resp, "not-found", httplib::status::not_found); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/regex/abc"));
            REQUIRE(resp.result() == httplib::status::not_found);
            REQUIRE(as_string(resp) == "not-found");
            co_return;
        });
}

TEST_CASE("Router: regex pattern with catastrophic backtracking shape is rejected", "[router]")
{
    net::thread_pool pool { 1 };
    httplib::server::http_server server(pool.get_executor());
    auto noop = [](httplib::server::request&, httplib::server::response&) {};

    // 量词嵌套：经典 ReDoS 形态，注册期即应报错，而不是在请求期挂死线程。
    REQUIRE_THROWS_AS(server.router().set_http_handler<httplib::method::get>("/a/{id:(a+)+}", noop),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(server.router().set_http_handler<httplib::method::get>("/b/{id:(a*)*}", noop),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(server.router().set_http_handler<httplib::method::get>("/c/{id:^(a+)+$}", noop),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(server.router().set_http_handler<httplib::method::get>("/d/{id:(a{2,})+}", noop),
                      std::invalid_argument);

    // 被量词作用、且内部含交替的分组。
    REQUIRE_THROWS_AS(server.router().set_http_handler<httplib::method::get>("/e/{id:(a|aa)+}", noop),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(server.router().set_http_handler<httplib::method::get>("/f/{id:^(a|a?)+$}", noop),
                      std::invalid_argument);

    pool.join();
}

TEST_CASE("Router: safe regex patterns are still accepted", "[router]")
{
    net::thread_pool pool { 1 };
    httplib::server::http_server server(pool.get_executor());
    auto noop = [](httplib::server::request&, httplib::server::response&) {};

    // 无分组的字符类/量词/锚点。
    REQUIRE_NOTHROW(server.router().set_http_handler<httplib::method::get>("/a/{id:^\\d+$}", noop));
    REQUIRE_NOTHROW(server.router().set_http_handler<httplib::method::get>("/b/{id:^[a-z]{2,8}$}", noop));
    REQUIRE_NOTHROW(server.router().set_http_handler<httplib::method::get>("/c/{id:^[A-Z][a-z]*$}", noop));
    REQUIRE_NOTHROW(server.router().set_http_handler<httplib::method::get>("/d/{id:^\\w+\\-\\w+$}", noop));

    // 分组内无「量词 + 交替」危险组合：普通分组、分组量词、字符类里的元字符。
    REQUIRE_NOTHROW(server.router().set_http_handler<httplib::method::get>("/e/{id:^(abc)+$}", noop));
    REQUIRE_NOTHROW(server.router().set_http_handler<httplib::method::get>("/f/{id:^(a|b|c)$}", noop));
    REQUIRE_NOTHROW(server.router().set_http_handler<httplib::method::get>("/g/{id:^([0-9]+)$}", noop));
    REQUIRE_NOTHROW(server.router().set_http_handler<httplib::method::get>("/h/{id:^[()+*]{3}$}", noop));
    REQUIRE_NOTHROW(server.router().set_http_handler<httplib::method::get>("/i/{id:^\\(\\)\\*$}", noop));
    REQUIRE_NOTHROW(server.router().set_http_handler<httplib::method::get>("/j/{id:^(?:ab)+$}", noop));

    pool.join();
}

TEST_CASE("Router: over-long regex pattern is rejected", "[router]")
{
    net::thread_pool pool { 1 };
    httplib::server::http_server server(pool.get_executor());
    auto noop = [](httplib::server::request&, httplib::server::response&) {};

    std::string pattern = "^";
    pattern.append(300, 'a');
    pattern += "$";

    REQUIRE_THROWS_AS(server.router().set_http_handler<httplib::method::get>("/long/{id:" + pattern + "}", noop),
                      std::invalid_argument);

    pool.join();
}

TEST_CASE("Router: over-long subject is not regex matched", "[router]")
{
    // 病态 pattern 未被完全杜绝时，超长路径段应直接判定为不匹配（快速 404），
    // 而不是把无意义的回溯算到请求头上。
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/bounded/{id:^[a-z]+$}",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "matched"); });
            server.router().set_http_not_found_handler([](httplib::server::request&, httplib::server::response& resp)
                                                       { set_text(resp, "not-found", httplib::status::not_found); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            // 上限之内：正常匹配。
            auto ok = UNWRAP(co_await client.async_get("/bounded/abc"));
            REQUIRE(ok.result() == httplib::status::ok);
            REQUIRE(as_string(ok) == "matched");

            // 远超上限：不进入正则匹配，直接 404。
            auto too_long = UNWRAP(co_await client.async_get("/bounded/" + std::string(2000, 'a')));
            REQUIRE(too_long.result() == httplib::status::not_found);
            REQUIRE(as_string(too_long) == "not-found");
            co_return;
        });
}

TEST_CASE("Router: wildcard path *", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
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
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(as_string(resp) == "sub/deep/file.txt");
            co_return;
        });
}

TEST_CASE("Router: percent-encoded slash does not separate segments", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/api/secret",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "secret"); });
            server.router().template set_http_handler<httplib::method::get>(
                "/api/*",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "wildcard"); });
            server.router().template set_http_handler<httplib::method::get>(
                "/users/:id",
                [](httplib::server::request& req, httplib::server::response& resp)
                { set_text(resp, "id=" + std::string(req.path_param("id"))); });
            // 注册 key 使用解码后的字面文本。
            server.router().template set_http_handler<httplib::method::get>(
                "/space/a b",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "space"); });
            server.router().set_http_not_found_handler(
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "not-found", httplib::status::not_found); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            // %2F 是段内容，不能命中 /api/secret，也不能命中 /api/*。
            auto r1 = UNWRAP(co_await client.async_get("/api%2Fsecret"));
            REQUIRE(r1.result() == httplib::status::not_found);
            REQUIRE(as_string(r1) == "not-found");

            auto r2 = UNWRAP(co_await client.async_get("/api%2Fother"));
            REQUIRE(r2.result() == httplib::status::not_found);
            REQUIRE(as_string(r2) == "not-found");

            // 未命中路由时是 404，而不是「命中节点但方法不允许」的 405。
            auto r3 = UNWRAP(co_await client.async_post("/api%2Fsecret", std::string_view(""), "text/plain"sv));
            REQUIRE(r3.result() == httplib::status::not_found);

            // :id 捕获到的是一个完整段，解码后可以包含 '/'。
            auto r4 = UNWRAP(co_await client.async_get("/users/a%2Fb"));
            REQUIRE(r4.result() == httplib::status::ok);
            REQUIRE(as_string(r4) == "id=a/b");

            // 十六进制大小写不敏感。
            auto r5 = UNWRAP(co_await client.async_get("/users/a%2fb"));
            REQUIRE(r5.result() == httplib::status::ok);
            REQUIRE(as_string(r5) == "id=a/b");

            // 只解码一次：%252F -> "%2F"，不会再变成 "/"。
            auto r6 = UNWRAP(co_await client.async_get("/users/a%252Fb"));
            REQUIRE(r6.result() == httplib::status::ok);
            REQUIRE(as_string(r6) == "id=a%2Fb");

            // 真正的两段路径仍然不匹配单参数路由。
            auto r7 = UNWRAP(co_await client.async_get("/users/a/b"));
            REQUIRE(r7.result() == httplib::status::not_found);

            // 普通静态路由不受影响。
            auto r8 = UNWRAP(co_await client.async_get("/api/secret"));
            REQUIRE(r8.result() == httplib::status::ok);
            REQUIRE(as_string(r8) == "secret");

            // 注册 key 仍是解码后的字面文本：%20 能匹配 key 中的空格。
            auto r9 = UNWRAP(co_await client.async_get("/space/a%20b"));
            REQUIRE(r9.result() == httplib::status::ok);
            REQUIRE(as_string(r9) == "space");

            co_return;
        });
}

TEST_CASE("Router: path(), decoded_path() and decoded_segments()", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/path/:id",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    std::string segments;
                    for (auto const& seg : req.decoded_segments())
                    {
                        segments += "[" + seg + "]";
                    }

                    set_text(resp,
                             std::string(req.path()) + "|" + req.decoded_path() + "|"
                                 + std::string(req.path_param("id")) + "|" + segments);
                });
            server.router().template set_http_handler<httplib::method::get>(
                "/segments/*",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    std::string segments;
                    for (auto const& seg : req.decoded_segments())
                    {
                        segments += "[" + seg + "]";
                    }
                    set_text(resp, segments);
                });
            server.router().template set_http_handler<httplib::method::get>(
                "/",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    std::string segments;
                    for (auto const& seg : req.decoded_segments())
                    {
                        segments += "[" + seg + "]";
                    }
                    set_text(resp, segments);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto r1 = UNWRAP(co_await client.async_get("/path/a%2Fb?x=1"));
            REQUIRE(r1.result() == httplib::status::ok);
            REQUIRE(as_string(r1) == "/path/a%2Fb|/path/a/b|a/b|[path][a/b]");

            auto r2 = UNWRAP(co_await client.async_get("/path/a%20b"));
            REQUIRE(r2.result() == httplib::status::ok);
            REQUIRE(as_string(r2) == "/path/a%20b|/path/a b|a b|[path][a b]");

            // 尾随 '/' 按路由既有规则产生一个空段。
            auto r3 = UNWRAP(co_await client.async_get("/segments/a%2Fb/"));
            REQUIRE(r3.result() == httplib::status::ok);
            REQUIRE(as_string(r3) == "[segments][a/b][]");

            // 根路径只有一个空段，不再产生两个。
            auto root = UNWRAP(co_await client.async_get("/"));
            REQUIRE(root.result() == httplib::status::ok);
            REQUIRE(as_string(root) == "[]");

            co_return;
        });
}

TEST_CASE("Router: multiple HTTP verbs on one route", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get, httplib::method::post>(
                "/multi-verb",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    if (req.method() == httplib::method::get)
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
            REQUIRE(resp_get.result() == httplib::status::ok);
            REQUIRE(as_string(resp_get) == "get-response");

            auto resp_post = UNWRAP(co_await client.async_post("/multi-verb", std::string_view(""), "text/plain"sv));
            REQUIRE(resp_post.result() == httplib::status::ok);
            REQUIRE(as_string(resp_post) == "post-response");
            co_return;
        });
}

TEST_CASE("Router: multiple named parameters", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
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
            auto resp = UNWRAP(co_await client.async_get("/blog/2024/12/hello-world"));
            REQUIRE(resp.result() == httplib::status::ok);
            co_return;
        });
}

TEST_CASE("Router: path_param template overload", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
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
            auto resp = UNWRAP(co_await client.async_get("/user/42/order/19.99"));
            REQUIRE(resp.result() == httplib::status::ok);
            co_return;
        });
}

TEST_CASE("Router: set_post_routing_handler for CORS", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::options>(
                "/*",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set("Access-Control-Allow-Origin", "*");
                    resp.set("Access-Control-Allow-Methods", "GET, POST");
                    resp.set_empty_content(httplib::status::no_content);
                });
            server.router().set_post_routing_handler(
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set("Access-Control-Allow-Origin", "*");
                    resp.set("Access-Control-Allow-Methods", "GET, POST");
                });
            server.router().template set_http_handler<httplib::method::post>(
                "/api/data",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "data-ok"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/api/data", std::string_view("{}"), "application/json"sv));
            REQUIRE(resp.result() == httplib::status::ok);
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
    run([&](auto& server)
        { server.router().template set_http_handler<httplib::method::get>("/member/:name", &test_handler::handle, th); },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/member/test"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(as_string(resp) == "member-test");
            co_return;
        });
}

TEST_CASE("Router: 405 Method Not Allowed", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/readonly",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "get-only"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/readonly", std::string_view(""), "text/plain"sv));
            REQUIRE(resp.result() == httplib::status::method_not_allowed);
            REQUIRE(resp["Allow"] == "GET");
            co_return;
        });
}

TEST_CASE("Router: static path priority over param", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/users/all",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "all-users"); });
            server.router().template set_http_handler<httplib::method::get>(
                "/users/:id",
                [](httplib::server::request& req, httplib::server::response& resp)
                { set_text(resp, "user-" + std::string(req.path_param("id"))); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/users/all"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(as_string(resp) == "all-users");

            auto resp2 = UNWRAP(co_await client.async_get("/users/42"));
            REQUIRE(resp2.result() == httplib::status::ok);
            REQUIRE(as_string(resp2) == "user-42");
            co_return;
        });
}

TEST_CASE("Router: regex param error in pre_routing", "[router]")
{
    net::thread_pool pool { 1 };
    httplib::server::http_server server(pool.get_executor());
    REQUIRE_THROWS_AS(
        server.router().set_http_handler<httplib::method::get>("/bad/{id:[}",
                                                          [](httplib::server::request&, httplib::server::response&) {}),
        std::regex_error);
    pool.join();
}

TEST_CASE("Router: trailing slash exact match", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/page/",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "slash"); });
            server.router().template set_http_handler<httplib::method::get>(
                "/page",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "no-slash"); });
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
    run([](auto&) {},
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/nonexistent"));
            REQUIRE(resp.result() == httplib::status::not_found);
            co_return;
        });
}

TEST_CASE("Router: strict segment split preserves spaces", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/strict/ a /",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "spaced"); });
            server.router().template set_http_handler<httplib::method::get>(
                "/strict/a/",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            // 字面空格是段内容，不能 trim 后命中 /strict/a/。
            auto spaced = UNWRAP(co_await client.async_get("/strict/%20a%20/"));
            REQUIRE(spaced.result() == httplib::status::ok);
            REQUIRE(as_string(spaced) == "spaced");

            auto plain = UNWRAP(co_await client.async_get("/strict/a/"));
            REQUIRE(plain.result() == httplib::status::ok);
            REQUIRE(as_string(plain) == "plain");
            co_return;
        });
}

TEST_CASE("Router: repeated slash is a distinct segment", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/dup/a/b",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "single"); });
            server.router().template set_http_handler<httplib::method::get>(
                "/dup/a//b",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "double"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto single = UNWRAP(co_await client.async_get("/dup/a/b"));
            REQUIRE(single.result() == httplib::status::ok);
            REQUIRE(as_string(single) == "single");

            auto doubled = UNWRAP(co_await client.async_get("/dup/a//b"));
            REQUIRE(doubled.result() == httplib::status::ok);
            REQUIRE(as_string(doubled) == "double");
            co_return;
        });
}

TEST_CASE("Router: static dead end falls back to param", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/fallback/all/end",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "static"); });
            server.router().template set_http_handler<httplib::method::get>(
                "/fallback/:id/other",
                [](httplib::server::request& req, httplib::server::response& resp)
                { set_text(resp, "param-" + std::string(req.path_param("id"))); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto static_hit = UNWRAP(co_await client.async_get("/fallback/all/end"));
            REQUIRE(static_hit.result() == httplib::status::ok);
            REQUIRE(as_string(static_hit) == "static");

            // 静态分支 all 走到 other 是死路，必须回退到 :id。
            auto param_hit = UNWRAP(co_await client.async_get("/fallback/all/other"));
            REQUIRE(param_hit.result() == httplib::status::ok);
            REQUIRE(as_string(param_hit) == "param-all");
            co_return;
        });
}

TEST_CASE("Router: same class routes keep registration order", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ordered/:first",
                [](httplib::server::request& req, httplib::server::response& resp)
                { set_text(resp, "first-" + std::string(req.path_param("first"))); });
            server.router().template set_http_handler<httplib::method::get>(
                "/ordered/:second",
                [](httplib::server::request& req, httplib::server::response& resp)
                { set_text(resp, "second-" + std::string(req.path_param("second"))); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/ordered/x"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(as_string(resp) == "first-x");
            co_return;
        });
}

TEST_CASE("Router: regex route wins over param route", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/priority/:id",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "param"); });
            server.router().template set_http_handler<httplib::method::get>(
                "/priority/{id:^\\d+$}",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "regex"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto regex_hit = UNWRAP(co_await client.async_get("/priority/42"));
            REQUIRE(regex_hit.result() == httplib::status::ok);
            REQUIRE(as_string(regex_hit) == "regex");

            auto param_hit = UNWRAP(co_await client.async_get("/priority/abc"));
            REQUIRE(param_hit.result() == httplib::status::ok);
            REQUIRE(as_string(param_hit) == "param");
            co_return;
        });
}

TEST_CASE("Router: 405 collects methods from all matching branches", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/allow/:id",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "get"); });
            server.router().template set_http_handler<httplib::method::post>(
                "/allow/{id:^\\d+$}",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "post"); });
            server.router().template set_http_handler<httplib::method::patch>(
                "/allow/42",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "patch"); });
            server.router().template set_http_handler<httplib::method::delete_>(
                "/allow/*",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "delete"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            // 静态 /allow/42、regex、param、wildcard 都匹配该 URL；静态分支没有 PUT
            // 时必须回退，并把四类分支的方法并入同一个 Allow。
            auto resp = UNWRAP(co_await client.async_put("/allow/42", std::string_view(""), "text/plain"sv));
            REQUIRE(resp.result() == httplib::status::method_not_allowed);
            REQUIRE(resp["Allow"] == "DELETE,GET,PATCH,POST");
            co_return;
        });
}

TEST_CASE("Router: wildcard preserves leading empty segment", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/wild/*",
                [](httplib::server::request& req, httplib::server::response& resp)
                { set_text(resp, "[" + std::string(req.path_param("*")) + "]"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto leading = UNWRAP(co_await client.async_get("/wild//x"));
            REQUIRE(leading.result() == httplib::status::ok);
            REQUIRE(as_string(leading) == "[/x]");

            auto middle = UNWRAP(co_await client.async_get("/wild/a//b"));
            REQUIRE(middle.result() == httplib::status::ok);
            REQUIRE(as_string(middle) == "[a//b]");
            co_return;
        });
}

TEST_CASE("Router: wildcard requires at least one segment", "[router]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/files/*",
                [](httplib::server::request& req, httplib::server::response& resp)
                { set_text(resp, "[" + std::string(req.path_param("*")) + "]"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            // 尾斜杠产生一个空段，因此 /files/ 命中；/files 没有剩余段。
            auto slash = UNWRAP(co_await client.async_get("/files/"));
            REQUIRE(slash.result() == httplib::status::ok);
            REQUIRE(as_string(slash) == "[]");

            auto bare = UNWRAP(co_await client.async_get("/files"));
            REQUIRE(bare.result() == httplib::status::not_found);
            co_return;
        });
}
