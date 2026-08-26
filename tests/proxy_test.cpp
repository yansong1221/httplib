#include "common.hpp"
#include "httplib/client/proxy_client.hpp"
#include "httplib/client/ws_client.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <thread>

namespace body = httplib::body;
namespace net = httplib::net;

using namespace httplib;

namespace
{

    std::string
    as_string(httplib::client::response& resp)
    {
        return resp.as_string();
    }

    template <typename M>
    std::string
    as_string(M& msg)
    {
        if (msg.body().template is_body_type<body::empty_body>())
            return {};
        return msg.body().template as<body::string_body>();
    }

    template <typename Setup, typename Test>
    void
    run_proxy(Setup&& setup, Test&& test)
    {
        net::thread_pool pool{ 2 };
        std::exception_ptr err;
        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void>
            {
                server::http_server upstream(pool.get_executor());
                server::http_server proxy(pool.get_executor());

                setup(upstream);

                upstream.listen("127.0.0.1", 0);
                proxy.listen("127.0.0.1", 0);
                auto u_ep = upstream.local_endpoint();
                proxy.set_reverse_proxy(
                    "/api/*",
                    std::format("http://{}:{}", u_ep.address().to_string(), u_ep.port()));
                upstream.run();
                proxy.run();

                client::http_client upstream_client(pool.get_executor(),
                                                     u_ep.address().to_string(),
                                                     u_ep.port());
                upstream_client.set_timeout(std::chrono::seconds(5));

                client::http_client proxy_client(pool.get_executor(),
                                                  "127.0.0.1",
                                                  proxy.local_endpoint().port());
                proxy_client.set_timeout(std::chrono::seconds(10));

                co_await test(upstream_client, proxy_client);
                upstream.stop();
                proxy.stop();
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

// ===========================================================================
// Basic proxy forwarding
// ===========================================================================

TEST_CASE("proxy: GET forwards to upstream", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/resource",
                [](server::request&, server::response& resp)
                { resp.set_string_content(std::string("upstream-resource"), "text/plain"); });
        },
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await proxy_client.async_get("/api/resource"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "upstream-resource");
        });
}

TEST_CASE("proxy: POST with body forwards to upstream", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router()
                .template set_http_handler<http::verb::post>(
                    "/echo",
                    [](server::request& req, server::response& resp)
                    { resp.set_string_content(as_string(req), "text/plain"); });
        },
        [](auto& upstream_client, auto& proxy_client) -> net::awaitable<void>
        {
            auto direct = UNWRAP(
                co_await upstream_client.async_post("/echo", std::string_view("direct-post")));
            REQUIRE(direct.result() == http::status::ok);
            REQUIRE(as_string(direct) == "direct-post");

            auto resp = UNWRAP(
                co_await proxy_client.async_post("/api/echo", std::string_view("hello-proxy")));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "hello-proxy");
        });
}

TEST_CASE("proxy: GET empty body forwards to upstream", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router()
                .template set_http_handler<http::verb::get>(
                    "/echo",
                    [](server::request&, server::response& resp)
                    { resp.set_string_content(std::string(), "text/plain"); });
        },
        [](auto& upstream_client, auto& proxy_client) -> net::awaitable<void>
        {
            auto direct = UNWRAP(co_await upstream_client.async_get("/echo"));
            REQUIRE(direct.result() == http::status::ok);

            auto resp = UNWRAP(co_await proxy_client.async_get("/api/echo"));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("proxy: PUT with body forwards to upstream", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::put>(
                "/echo-put",
                [](server::request& req, server::response& resp)
                { resp.set_string_content(as_string(req), "text/plain"); });
        },
        [](auto& upstream_client, auto& proxy_client) -> net::awaitable<void>
        {
            auto direct = UNWRAP(
                co_await upstream_client.async_put("/echo-put", std::string_view("put-data")));
            REQUIRE(direct.result() == http::status::ok);
            REQUIRE(as_string(direct) == "put-data");

            auto resp = UNWRAP(
                co_await proxy_client.async_put("/api/echo-put", std::string_view("put-data")));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "put-data");
        });
}

// ===========================================================================
// Status code forwarding
// ===========================================================================

TEST_CASE("proxy: status code 201", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/status/:code",
                [](server::request& req, server::response& resp)
                {
                    auto code = std::stoi(std::string(req.path_param("code")));
                    resp.set_empty_content(static_cast<http::status>(code));
                });
        },
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await proxy_client.async_get("/api/status/201"));
            REQUIRE(resp.result() == http::status::created);
        });
}

TEST_CASE("proxy: status code 404", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/status/:code",
                [](server::request& req, server::response& resp)
                {
                    auto code = std::stoi(std::string(req.path_param("code")));
                    resp.set_empty_content(static_cast<http::status>(code));
                });
        },
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await proxy_client.async_get("/api/status/404"));
            REQUIRE(resp.result() == http::status::not_found);
        });
}

TEST_CASE("proxy: status code 204", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/status/:code",
                [](server::request& req, server::response& resp)
                {
                    auto code = std::stoi(std::string(req.path_param("code")));
                    resp.set_empty_content(static_cast<http::status>(code));
                });
        },
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await proxy_client.async_get("/api/status/204"));
            REQUIRE(resp.result() == http::status::no_content);
        });
}

TEST_CASE("proxy: status code 304", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/status/:code",
                [](server::request& req, server::response& resp)
                {
                    auto code = std::stoi(std::string(req.path_param("code")));
                    resp.set_empty_content(static_cast<http::status>(code));
                });
        },
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await proxy_client.async_get("/api/status/304"));
            REQUIRE(resp.result() == http::status::not_modified);
        });
}

TEST_CASE("proxy: status code 102 (1xx)", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/status/:code",
                [](server::request& req, server::response& resp)
                {
                    auto code = std::stoi(std::string(req.path_param("code")));
                    resp.set_empty_content(static_cast<http::status>(code));
                });
        },
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await proxy_client.async_get("/api/status/102"));
            REQUIRE(resp.result() == http::status::processing);
        });
}

// ===========================================================================
// Header forwarding
// ===========================================================================

TEST_CASE("proxy: X-Forwarded-For is appended", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/headers",
                [](server::request& req, server::response& resp)
                {
                    auto xff = req["X-Forwarded-For"];
                    resp.set_string_content(std::string(xff), "text/plain");
                });
        },
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await proxy_client.async_get("/api/headers"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp).find("127.0.0.1") != std::string::npos);
        });
}

TEST_CASE("proxy: Host header set to upstream host", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/check-host",
                [](server::request& req, server::response& resp)
                { resp.set_string_content(std::string(req["Host"]), "text/plain"); });
        },
        [](auto& upstream_client, auto& proxy_client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await proxy_client.async_get("/api/check-host"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp)
                    == std::format("{}:{}", upstream_client.host(), upstream_client.port()));
        });
}

// ===========================================================================
// Error paths
// ===========================================================================

TEST_CASE("proxy: path not matching prefix returns 404", "[proxy]")
{
    run_proxy(
        [](auto&) {},
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await proxy_client.async_get("/other/resource"));
            REQUIRE(resp.result() == http::status::not_found);
        });
}

TEST_CASE("proxy: empty Content-Length:0 response", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/empty-ok",
                [](server::request&, server::response& resp)
                { resp.set_string_content(std::string(), "text/plain"); });
        },
        [](auto& upstream_client, auto& proxy_client) -> net::awaitable<void>
        {
            auto direct = UNWRAP(co_await upstream_client.async_get("/empty-ok"));
            REQUIRE(direct.result() == http::status::ok);
            REQUIRE(as_string(direct).empty());

            auto resp = UNWRAP(co_await proxy_client.async_get("/api/empty-ok"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp).empty());
        });
}

// ===========================================================================
// Large body / repeated requests
// ===========================================================================

TEST_CASE("proxy: POST large body", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::post>(
                "/body-size",
                [](server::request& req, server::response& resp)
                {
                    auto body = as_string(req);
                    resp.set_string_content(std::to_string(body.size()), "text/plain");
                });
        },
        [](auto& upstream_client, auto& proxy_client) -> net::awaitable<void>
        {
            std::string large_body(10000, 'x');
            auto direct = UNWRAP(
                co_await upstream_client.async_post("/body-size", large_body));
            REQUIRE(direct.result() == http::status::ok);
            REQUIRE(as_string(direct) == "10000");

            auto resp = UNWRAP(
                co_await proxy_client.async_post("/api/body-size", large_body));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "10000");
        });
}

TEST_CASE("proxy: POST with empty body", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router()
                .template set_http_handler<http::verb::post>(
                    "/echo",
                    [](server::request& req, server::response& resp)
                    { resp.set_string_content(as_string(req), "text/plain"); });
        },
        [](auto& upstream_client, auto& proxy_client) -> net::awaitable<void>
        {
            auto direct = UNWRAP(
                co_await upstream_client.async_post("/echo", std::string_view("")));
            REQUIRE(direct.result() == http::status::ok);

            auto resp = UNWRAP(
                co_await proxy_client.async_post("/api/echo", std::string_view("")));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("proxy: repeated requests", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router()
                .template set_http_handler<http::verb::get, http::verb::post>(
                    "/echo",
                    [](server::request& req, server::response& resp)
                    { resp.set_string_content(as_string(req), "text/plain"); });
            upstream.router().template set_http_handler<http::verb::get>(
                "/resource",
                [](server::request&, server::response& resp)
                { resp.set_string_content(std::string("upstream-resource"), "text/plain"); });
        },
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            for (int i = 0; i < 5; ++i)
            {
                auto resp1 = UNWRAP(co_await proxy_client.async_get("/api/resource"));
                REQUIRE(resp1.result() == http::status::ok);
                REQUIRE(as_string(resp1) == "upstream-resource");

                auto resp2 = UNWRAP(
                    co_await proxy_client.async_post(
                        "/api/echo", std::string_view("p" + std::to_string(i))));
                REQUIRE(resp2.result() == http::status::ok);
                REQUIRE(as_string(resp2) == "p" + std::to_string(i));
            }
        });
}

TEST_CASE("proxy: status 200/404/204 on same connection", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/status/:code",
                [](server::request& req, server::response& resp)
                {
                    auto code = std::stoi(std::string(req.path_param("code")));
                    resp.set_empty_content(static_cast<http::status>(code));
                });
        },
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            for (int i = 0; i < 5; ++i)
            {
                auto r = UNWRAP(co_await proxy_client.async_get("/api/status/200"));
                REQUIRE(r.result() == http::status::ok);
                r = UNWRAP(co_await proxy_client.async_get("/api/status/404"));
                REQUIRE(r.result() == http::status::not_found);
                r = UNWRAP(co_await proxy_client.async_get("/api/status/204"));
                REQUIRE(r.result() == http::status::no_content);
            }
        });
}

TEST_CASE("proxy: mixed requests with body then status", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router()
                .template set_http_handler<http::verb::get, http::verb::post>(
                    "/echo",
                    [](server::request& req, server::response& resp)
                    { resp.set_string_content(as_string(req), "text/plain"); });
            upstream.router().template set_http_handler<http::verb::get>(
                "/status/:code",
                [](server::request& req, server::response& resp)
                {
                    auto code = std::stoi(std::string(req.path_param("code")));
                    resp.set_empty_content(static_cast<http::status>(code));
                });
        },
        [](auto&, auto& proxy_client) -> net::awaitable<void>
        {
            for (int i = 0; i < 5; ++i)
            {
                auto r = UNWRAP(co_await proxy_client.async_post(
                    "/api/echo", std::string_view("b" + std::to_string(i))));
                REQUIRE(r.result() == http::status::ok);
                r = UNWRAP(co_await proxy_client.async_get("/api/status/404"));
                REQUIRE(r.result() == http::status::not_found);
                r = UNWRAP(co_await proxy_client.async_get("/api/status/200"));
                REQUIRE(r.result() == http::status::ok);
            }
        });
}

// ===========================================================================
// Redirect proxying
// ===========================================================================

TEST_CASE("proxy: redirect (301) proxying", "[proxy]")
{
    run_proxy(
        [](auto& upstream)
        {
            upstream.router().template set_http_handler<http::verb::get>(
                "/redirect",
                [](server::request&, server::response& resp)
                { resp.set_redirect("/resource", http::status::moved_permanently); });
        },
        [](auto& upstream_client, auto& proxy_client) -> net::awaitable<void>
        {
            auto direct = UNWRAP(co_await upstream_client.async_get("/redirect"));
            REQUIRE(direct.result() == http::status::moved_permanently);

            auto resp = UNWRAP(co_await proxy_client.async_get("/api/redirect"));
            REQUIRE(resp.result() == http::status::moved_permanently);
        });
}

// ===========================================================================
// Proxy rewrites
// ===========================================================================

TEST_CASE("proxy: rewrites Cookie header", "[proxy]")
{
    net::thread_pool pool{ 2 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            server::http_server upstream(pool.get_executor());
            server::http_server proxy(pool.get_executor());

            upstream.listen("127.0.0.1", 0);
            proxy.listen("127.0.0.1", 0);
            auto u_ep = upstream.local_endpoint();
            auto u_host = u_ep.address().to_string();
            auto u_port = u_ep.port();
            proxy.set_reverse_proxy("/api/*",
                                    std::format("http://{}:{}", u_host, u_port));

            upstream.router().template set_http_handler<http::verb::get>(
                "/check-cookie",
                [](server::request& req, server::response& resp)
                { resp.set_string_content(std::string(req["Cookie"]), "text/plain"); });

            upstream.run();
            proxy.run();

            client::http_client c(pool.get_executor(), "127.0.0.1",
                                   proxy.local_endpoint().port());
            c.set_timeout(std::chrono::seconds(5));

            auto hdrs = http::fields();
            hdrs.set(http::field::cookie, "token=abc; Domain=upstream.com; Path=/api");
            auto resp = UNWRAP(
                co_await c.async_send_request(httplib::client::request(http::verb::get, "/api/check-cookie", hdrs)));
            REQUIRE(resp.result() == http::status::ok);
            auto body = as_string(resp);
            REQUIRE(body.find("token=abc") != std::string::npos);
            REQUIRE(body.find("Domain=upstream.com") == std::string::npos);
            REQUIRE(body.find("Path=/api") == std::string::npos);
            REQUIRE(body.find("Domain=" + u_host) != std::string::npos);
            REQUIRE(body.find("Path=/") != std::string::npos);

            upstream.stop();
            proxy.stop();
        },
        [&](std::exception_ptr e)
        {
            err = e;
        });
    pool.join();
    if (err)
        std::rethrow_exception(err);
}

TEST_CASE("proxy: rewrites Referer to upstream", "[proxy]")
{
    net::thread_pool pool{ 2 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            server::http_server upstream(pool.get_executor());
            server::http_server proxy(pool.get_executor());

            upstream.listen("127.0.0.1", 0);
            proxy.listen("127.0.0.1", 0);
            auto u_ep = upstream.local_endpoint();
            auto u_host = u_ep.address().to_string();
            auto u_port = u_ep.port();
            auto p_port = proxy.local_endpoint().port();
            proxy.set_reverse_proxy("/api/*",
                                    std::format("http://{}:{}", u_host, u_port));

            upstream.router().template set_http_handler<http::verb::get>(
                "/echo-referer",
                [](server::request& req, server::response& resp)
                { resp.set_string_content(std::string(req[http::field::referer]), "text/plain"); });

            upstream.run();
            proxy.run();

            client::http_client c(pool.get_executor(), "127.0.0.1", p_port);
            c.set_timeout(std::chrono::seconds(5));

            auto hdrs = http::fields();
            hdrs.set(http::field::referer,
                      std::format("http://127.0.0.1:{}/api/some-page?a=1&b=2#sec", p_port));
            auto resp = UNWRAP(
                co_await c.async_send_request(httplib::client::request(http::verb::get, "/api/echo-referer", hdrs)));
            REQUIRE(resp.result() == http::status::ok);
            auto body = as_string(resp);
            REQUIRE(body.find(u_host) != std::string::npos);
            REQUIRE(body.find("/some-page?a=1&b=2#sec") != std::string::npos);
            REQUIRE(body.find("/api") == std::string::npos);

            upstream.stop();
            proxy.stop();
        },
        [&](std::exception_ptr e)
        {
            err = e;
        });
    pool.join();
    if (err)
        std::rethrow_exception(err);
}

TEST_CASE("proxy: forwards X-Forwarded-Proto and X-Forwarded-Host", "[proxy]")
{
    net::thread_pool pool{ 2 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            server::http_server upstream(pool.get_executor());
            server::http_server proxy(pool.get_executor());

            upstream.listen("127.0.0.1", 0);
            proxy.listen("127.0.0.1", 0);
            auto u_ep = upstream.local_endpoint();
            auto u_host = u_ep.address().to_string();
            auto u_port = u_ep.port();
            proxy.set_reverse_proxy("/api/*",
                                    std::format("http://{}:{}", u_host, u_port));

            upstream.router().template set_http_handler<http::verb::get>(
                "/echo-headers",
                [](server::request& req, server::response& resp)
                {
                    auto proto = req["X-Forwarded-Proto"];
                    auto host = req["X-Forwarded-Host"];
                    resp.set_string_content(
                        std::format("proto={} host={}", std::string(proto), std::string(host)),
                        "text/plain");
                });

            upstream.run();
            proxy.run();

            client::http_client c(pool.get_executor(), "127.0.0.1",
                                   proxy.local_endpoint().port());
            c.set_timeout(std::chrono::seconds(5));
            auto resp = UNWRAP(co_await c.async_get("/api/echo-headers"));
            REQUIRE(resp.result() == http::status::ok);
            auto body = as_string(resp);
            REQUIRE(body.find("proto=http") != std::string::npos);
            REQUIRE(body.find("host=127.0.0.1:") != std::string::npos);

            upstream.stop();
            proxy.stop();
        },
        [&](std::exception_ptr e)
        {
            err = e;
        });
    pool.join();
    if (err)
        std::rethrow_exception(err);
}

// ===========================================================================
// Redirect location rewriting
// ===========================================================================

TEST_CASE("proxy: rewrites redirect Location", "[proxy]")
{
    net::thread_pool pool{ 2 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            server::http_server upstream(pool.get_executor());
            server::http_server proxy(pool.get_executor());

            upstream.listen("127.0.0.1", 0);
            proxy.listen("127.0.0.1", 0);
            auto u_ep = upstream.local_endpoint();
            auto u_host = u_ep.address().to_string();
            auto u_port = u_ep.port();
            proxy.set_reverse_proxy("/api/*",
                                    std::format("http://{}:{}", u_host, u_port));

            upstream.router().template set_http_handler<http::verb::get>(
                "/redirect-me",
                [&](server::request&, server::response& resp)
                {
                    resp.set_redirect(
                        std::format("http://{}:{}/new-place", u_host, u_port),
                        http::status::moved_permanently);
                });
            upstream.router().template set_http_handler<http::verb::get>(
                "/redirect-root",
                [&](server::request&, server::response& resp)
                {
                    resp.set_redirect(std::format("http://{}:{}/", u_host, u_port),
                                      http::status::found);
                });

            upstream.run();
            proxy.run();

            client::http_client c(pool.get_executor(), "127.0.0.1",
                                   proxy.local_endpoint().port());
            c.set_timeout(std::chrono::seconds(5));
            c.set_max_redirects(0);

            auto resp = UNWRAP(co_await c.async_get("/api/redirect-me"));
            REQUIRE(resp.result() == http::status::moved_permanently);
            REQUIRE(std::string(resp["Location"]) == "/api/new-place");

            auto resp2 = UNWRAP(co_await c.async_get("/api/redirect-root"));
            REQUIRE(resp2.result() == http::status::found);
            REQUIRE(std::string(resp2["Location"]) == "/api/");

            upstream.stop();
            proxy.stop();
        },
        [&](std::exception_ptr e)
        {
            err = e;
        });
    pool.join();
    if (err)
        std::rethrow_exception(err);
}

TEST_CASE("proxy: rewrites redirect Location with base path", "[proxy]")
{
    net::thread_pool pool{ 2 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            server::http_server upstream(pool.get_executor());
            server::http_server proxy(pool.get_executor());

            upstream.listen("127.0.0.1", 0);
            proxy.listen("127.0.0.1", 0);
            auto u_ep = upstream.local_endpoint();
            auto u_host = u_ep.address().to_string();
            auto u_port = u_ep.port();
            proxy.set_reverse_proxy("/api/*",
                                    std::format("http://{}:{}/qqq", u_host, u_port));

            upstream.router().template set_http_handler<http::verb::get>(
                "/qqq/redirect-me",
                [&](server::request&, server::response& resp)
                {
                    resp.set_redirect(
                        std::format("http://{}:{}/new-place", u_host, u_port),
                        http::status::moved_permanently);
                });

            upstream.run();
            proxy.run();

            client::http_client c(pool.get_executor(), "127.0.0.1",
                                   proxy.local_endpoint().port());
            c.set_timeout(std::chrono::seconds(5));
            c.set_max_redirects(0);

            auto resp = UNWRAP(co_await c.async_get("/api/redirect-me"));
            REQUIRE(resp.result() == http::status::moved_permanently);
            REQUIRE(std::string(resp["Location"]) == "/api/new-place");

            upstream.stop();
            proxy.stop();
        },
        [&](std::exception_ptr e)
        {
            err = e;
        });
    pool.join();
    if (err)
        std::rethrow_exception(err);
}

// ===========================================================================
// Proxy interceptor
// ===========================================================================

TEST_CASE("proxy: interceptor all steps called", "[proxy]")
{
    net::thread_pool pool{ 2 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            server::http_server upstream(pool.get_executor());
            server::http_server proxy(pool.get_executor());

            upstream.listen("127.0.0.1", 0);
            proxy.listen("127.0.0.1", 0);
            auto u_ep = upstream.local_endpoint();
            auto p_ep = proxy.local_endpoint();
            auto u_url = std::format("http://{}:{}", u_ep.address().to_string(), u_ep.port());

            upstream.router().template set_http_handler<http::verb::post>(
                "/echo",
                [](server::request& req, server::response& resp)
                { resp.set_string_content(std::string(req.body().template as<body::string_body>()), "text/plain"); });

            std::atomic<int> req_called{ 0 }, req_body_called{ 0 }, resp_called{ 0 },
                resp_body_called{ 0 };

            struct test_interceptor : server::proxy_interceptor
            {
                std::atomic<int>* req_called;
                std::atomic<int>* req_body_called;
                std::atomic<int>* resp_called;
                std::atomic<int>* resp_body_called;

                net::awaitable<void>
                on_upstream_request(server::request&, http::fields&,
                                    std::string const&) override
                {
                    (*req_called)++;
                    co_return;
                }
                net::awaitable<void>
                on_upstream_request_body(net::const_buffer, bool) override
                {
                    (*req_body_called)++;
                    co_return;
                }
                net::awaitable<void>
                on_upstream_response(server::request&, http::status,
                                     http::fields const&) override
                {
                    (*resp_called)++;
                    co_return;
                }
                net::awaitable<void>
                on_upstream_response_body(net::const_buffer, bool) override
                {
                    (*resp_body_called)++;
                    co_return;
                }
            };

            proxy.set_reverse_proxy(
                "/api/*", u_url,
                [&](server::request&) -> std::shared_ptr<server::proxy_interceptor>
                {
                    auto ti = std::make_shared<test_interceptor>();
                    ti->req_called = &req_called;
                    ti->req_body_called = &req_body_called;
                    ti->resp_called = &resp_called;
                    ti->resp_body_called = &resp_body_called;
                    return ti;
                });

            upstream.run();
            proxy.run();

            client::http_client c(pool.get_executor(), p_ep.address().to_string(),
                                   p_ep.port());
            c.set_timeout(std::chrono::seconds(5));
            auto resp = UNWRAP(
                co_await c.async_post("/api/echo", std::string_view("hello")));

            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "hello");
            REQUIRE(req_called.load() == 1);
            REQUIRE(req_body_called.load() >= 1);
            REQUIRE(resp_called.load() == 1);
            REQUIRE(resp_body_called.load() >= 1);

            upstream.stop();
            proxy.stop();
        },
        [&](std::exception_ptr e)
        {
            err = e;
        });
    pool.join();
    if (err)
        std::rethrow_exception(err);
}

// ===========================================================================
// WebSocket forwarding
// ===========================================================================

TEST_CASE("ws-forward echo", "[proxy][ws-forward]")
{
    net::thread_pool ioc{ 2 };
    server::http_server upstream(ioc.get_executor());
    server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    proxy.listen("127.0.0.1", 0);
    upstream.run();
    proxy.run();

    auto upstream_ep = upstream.local_endpoint();
    auto proxy_ep = proxy.local_endpoint();
    std::string upstream_url
        = std::format("ws://{}:{}", upstream_ep.address().to_string(), upstream_ep.port());

    upstream.router().set_ws_handler(
        "/extra-path",
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [](server::websocket_conn::weak_ptr wp, std::string_view msg,
           bool binary) -> net::awaitable<void>
        {
            if (auto c = wp.lock())
                c->send(msg, binary);
            co_return;
        },
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });

    proxy.set_ws_forward("/ws/*", upstream_url);

    client::ws_client ws(ioc.get_executor(), proxy_ep.address().to_string(), proxy_ep.port());
    std::vector<std::string> received;
    std::string large_data(1024 * 64, 'X');
    for (std::size_t i = 0; i < large_data.size(); ++i)
        large_data[i] = static_cast<char>(i % 256);

    ws.run(
        "/ws/extra-path",
        [&](boost::system::error_code ec) -> net::awaitable<void>
        {
            REQUIRE(!ec);
            ws.send(std::string("hello-forward"));
            ws.send(std::string("message-two"));
            ws.send(std::string(large_data), true);
            ws.send(std::string("final"));
            co_return;
        },
        [&](std::string_view msg, bool binary) -> net::awaitable<void>
        {
            received.emplace_back(msg);
            if (received.size() >= 4)
                ws.close();
            co_return;
        },
        []() -> net::awaitable<void> { co_return; });

    std::this_thread::sleep_for(std::chrono::seconds(5));

    upstream.stop();
    proxy.stop();
    ioc.join();

    REQUIRE(received.size() == 4);
    REQUIRE(received[0] == "hello-forward");
    REQUIRE(received[1] == "message-two");
    REQUIRE(received[2] == large_data);
    REQUIRE(received[3] == "final");
}

TEST_CASE("ws-forward stress: concurrent connections + shutdown", "[proxy][ws-forward]")
{
    net::thread_pool ioc{ 2 };
    server::http_server upstream(ioc.get_executor());
    server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    proxy.listen("127.0.0.1", 0);
    upstream.run();
    proxy.run();

    auto upstream_ep = upstream.local_endpoint();
    auto proxy_ep = proxy.local_endpoint();
    std::string upstream_url
        = std::format("ws://{}:{}", upstream_ep.address().to_string(), upstream_ep.port());

    constexpr int kConnections = 6;
    std::atomic<int> total_sent{ 0 };
    std::atomic<int> total_recv{ 0 };
    std::atomic<bool> stop_flag{ false };

    upstream.router().set_ws_handler(
        "/echo",
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [&](server::websocket_conn::weak_ptr wp, std::string_view msg,
           bool binary) -> net::awaitable<void>
        {
            ++total_recv;
            if (auto c = wp.lock())
                c->send(msg, binary);
            co_return;
        },
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });

    proxy.set_ws_forward("/ws/*", upstream_url);

    std::vector<std::unique_ptr<client::ws_client>> clients;

    for (int i = 0; i < kConnections; ++i)
    {
        auto ws = std::make_unique<client::ws_client>(ioc.get_executor(),
                                                       proxy_ep.address().to_string(),
                                                       proxy_ep.port());
        clients.push_back(std::move(ws));
    }

    for (auto& c : clients)
    {
        c->run(
            "/ws/echo",
            [&, ws = c.get()](boost::system::error_code ec) -> net::awaitable<void>
            {
                if (!ec)
                {
                    for (int j = 0; j < 2000 && !stop_flag.load(); ++j)
                    {
                        ws->send(std::format("{}", j));
                        ++total_sent;
                    }
                }
                co_return;
            },
            [](std::string_view, bool) -> net::awaitable<void> { co_return; },
            [&]() -> net::awaitable<void>
            {
                co_return;
        });
}

    std::this_thread::sleep_for(std::chrono::seconds(3));
    stop_flag.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    upstream.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    proxy.stop();
    ioc.join();

    REQUIRE(total_sent.load() > 0);
    REQUIRE(total_recv.load() > 0);
}

TEST_CASE("ws-interceptor: messages intercepted", "[proxy][ws-forward]")
{
    net::thread_pool ioc{ 2 };
    server::http_server upstream(ioc.get_executor());
    server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    proxy.listen("127.0.0.1", 0);
    upstream.run();
    proxy.run();

    auto upstream_ep = upstream.local_endpoint();
    auto proxy_ep = proxy.local_endpoint();
    std::string upstream_url
        = std::format("ws://{}:{}", upstream_ep.address().to_string(), upstream_ep.port());

    upstream.router().set_ws_handler(
        "/echo",
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [](server::websocket_conn::weak_ptr wp, std::string_view msg,
           bool binary) -> net::awaitable<void>
        {
            if (auto c = wp.lock())
                c->send(msg, binary);
            co_return;
        },
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });

    std::atomic<int> req_called{ 0 }, client_msg_called{ 0 }, upstream_msg_called{ 0 };

    struct test_ws_interceptor : server::ws_interceptor
    {
        std::atomic<int>* req_called;
        std::atomic<int>* client_msg_called;
        std::atomic<int>* upstream_msg_called;

        net::awaitable<void>
        on_upstream_request(server::request&, http::fields&,
                            std::string const&) override
        {
            (*req_called)++;
            co_return;
        }
        net::awaitable<void>
        on_upstream_send(std::string_view, bool) override
        {
            (*client_msg_called)++;
            co_return;
        }
        net::awaitable<void>
        on_upstream_recv(std::string_view, bool) override
        {
            (*upstream_msg_called)++;
            co_return;
        }
    };

    proxy.set_ws_forward(
        "/ws/*", upstream_url,
        [&](server::request&) -> std::shared_ptr<server::ws_interceptor>
        {
            auto ti = std::make_shared<test_ws_interceptor>();
            ti->req_called = &req_called;
            ti->client_msg_called = &client_msg_called;
            ti->upstream_msg_called = &upstream_msg_called;
            return ti;
        });

    client::ws_client ws(ioc.get_executor(), proxy_ep.address().to_string(), proxy_ep.port());
    std::vector<std::string> received;

    ws.run(
        "/ws/echo",
        [&](boost::system::error_code ec) -> net::awaitable<void>
        {
            REQUIRE(!ec);
            ws.send(std::string("hello"));
            ws.send(std::string("world"));
            ws.send(std::string("done"));
            co_return;
        },
        [&](std::string_view msg, bool) -> net::awaitable<void>
        {
            received.emplace_back(msg);
            if (received.size() >= 3)
                ws.close();
            co_return;
        },
        []() -> net::awaitable<void> { co_return; });

    std::this_thread::sleep_for(std::chrono::seconds(5));
    upstream.stop();
    proxy.stop();
    ioc.join();

    REQUIRE(received.size() == 3);
    REQUIRE(received[0] == "hello");
    REQUIRE(received[1] == "world");
    REQUIRE(received[2] == "done");
    REQUIRE(req_called.load() == 1);
    REQUIRE(client_msg_called.load() == 3);
    REQUIRE(upstream_msg_called.load() == 3);
}

TEST_CASE("CONNECT: rejected by default", "[proxy]")
{
    using test_common::run;
    using test_common::setup_logger;

    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/",
                [](server::request&, server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::connect, "example.com:80")));
            REQUIRE(resp.result() == http::status::method_not_allowed);
            co_return;
        });
}

TEST_CASE("CONNECT: route handler can approve", "[proxy]")
{
    using test_common::run;
    using test_common::setup_logger;

    bool handler_called = false;

    run(
        [&](auto& server)
        {
            server.router().set_connect_handler(
                "example.com:80",
                [&](server::request& req, server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.target() == "example.com:80");
                    handler_called = true;
                    co_return;
                });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            co_await client.async_send_request(httplib::client::request(http::verb::connect, "example.com:80"));
            REQUIRE(handler_called);
            co_return;
        });
}

TEST_CASE("CONNECT: route handler can reject", "[proxy]")
{
    using test_common::run;
    using test_common::setup_logger;

    run(
        [](auto& server)
        {
            server.router().set_connect_handler(
                "example.com:80",
                [](server::request& req, server::response& resp) -> net::awaitable<void>
                {
                    resp.set_error_content(http::status::forbidden);
                    co_return;
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::connect, "example.com:80")));
            REQUIRE(resp.result() == http::status::forbidden);
            co_return;
        });
}

TEST_CASE("CONNECT: wildcard handler matches any target", "[proxy]")
{
    using test_common::run;
    using test_common::setup_logger;

    bool handler_called = false;

    run(
        [&](auto& server)
        {
            server.router().set_connect_handler(
                "*",
                [&](server::request& req, server::response& resp) -> net::awaitable<void>
                {
                    handler_called = true;
                    co_return;
                });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            co_await client.async_send_request(httplib::client::request(http::verb::connect, "any.host:443"));
            REQUIRE(handler_called);
            co_return;
        });
}

TEST_CASE("CONNECT: path-specific handlers don't interfere", "[proxy]")
{
    using test_common::run;
    using test_common::setup_logger;

    run(
        [](auto& server)
        {
            server.router().set_connect_handler(
                "allowed.host:80",
                [](server::request&, server::response&) -> net::awaitable<void>
                { co_return; });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::connect, "other.host:80")));
            REQUIRE(resp.result() == http::status::method_not_allowed);
            co_return;
        });
}

TEST_CASE("CONNECT: tunnel forwards data bidirectionally", "[proxy]")
{
    net::io_context ioc;

    tcp::acceptor echo_acceptor(ioc);
    echo_acceptor.open(tcp::v4());
    echo_acceptor.bind(tcp::endpoint(net::ip::make_address("127.0.0.1"), 0));
    echo_acceptor.listen();
    auto echo_port = echo_acceptor.local_endpoint().port();

    std::atomic<int> echo_recv_bytes { -1 };
    std::string echo_data;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sock = co_await echo_acceptor.async_accept(net::use_awaitable);
            std::vector<char> buf(64);
            boost::system::error_code ec;
            auto n = co_await sock.async_read_some(net::buffer(buf), net::use_awaitable);
            echo_recv_bytes = static_cast<int>(n);
            echo_data = std::string(buf.data(), n);
            if (n > 0)
            {
                co_await net::async_write(sock, net::buffer(buf, n), net::use_awaitable);
            }
            sock.shutdown(tcp::socket::shutdown_send, ec);
            sock.close(ec);
        },
        net::detached);

    httplib::server::http_server proxy(ioc.get_executor());
    proxy.router().set_connect_handler(
        "*",
        [](httplib::server::request&, httplib::server::response&) -> net::awaitable<void> { co_return; });
    proxy.listen("127.0.0.1", 0);
    auto proxy_ep = proxy.local_endpoint();
    proxy.run();

    bool tunnel_ok = false;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            httplib::client::proxy_client pc(
                ioc.get_executor(), proxy_ep.address().to_string(), proxy_ep.port());

            auto ec = co_await pc.async_connect(std::format("127.0.0.1:{}", echo_port));
            if (ec)
            {
                proxy.stop();
                co_return;
            }

            co_await pc.async_write(net::buffer(std::string("ping")));

            std::vector<char> recv(64);
            auto result = co_await pc.async_read_some(net::buffer(recv));
            if (result.has_value()
                && result.value() == 4
                && std::string_view(recv.data(), 4) == "ping")
            {
                tunnel_ok = true;
            }

            pc.close();
            proxy.stop();
        },
        net::detached);

    ioc.run();

    REQUIRE(tunnel_ok);
    REQUIRE(echo_recv_bytes.load() == 4);
    REQUIRE(echo_data == "ping");
}
