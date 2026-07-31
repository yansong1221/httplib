#include "httplib/client/client.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace httplib;

namespace {

struct test_scaffold
{
    net::io_context ioc;
    server::http_server upstream_server;
    server::http_server proxy_server;

    std::unique_ptr<client::http_client> upstream_client;
    std::unique_ptr<client::http_client> proxy_client;

    std::string upstream_host;
    uint16_t upstream_port = 0;
    uint16_t proxy_port    = 0;

    std::thread worker;

    test_scaffold()
        : upstream_server(ioc)
        , proxy_server(ioc)
    {
    }

    void start()
    {
        upstream_server.run();
        proxy_server.run();
        worker = std::thread([this] { ioc.run(); });

        upstream_client = std::make_unique<client::http_client>(
            ioc.get_executor(), upstream_host, upstream_port);
        upstream_client->set_timeout(std::chrono::seconds(5));

        proxy_client = std::make_unique<client::http_client>(
            ioc.get_executor(), "127.0.0.1", proxy_port);
        proxy_client->set_timeout(std::chrono::seconds(10));
    }

    ~test_scaffold()
    {
        upstream_server.stop().wait();
        proxy_server.stop().wait();
        ioc.stop();
        if (worker.joinable())
            worker.join();
    }
};

#define UNWRAP(result)                      \
    [&](auto&& r) {                         \
        REQUIRE(r.has_value());             \
        return std::move(std::move(r).value()); \
    }(result)

std::string as_string(auto& msg)
{
    if (msg.body().is_body_type<body::empty_body>())
        return {};
    return msg.body().as<body::string_body>();
}

} // namespace

TEST_CASE("reverse-proxy", "[proxy]")
{
    test_scaffold ts;

    ts.upstream_server.listen("127.0.0.1", 0);
    ts.upstream_host = ts.upstream_server.local_endpoint().address().to_string();
    ts.upstream_port = ts.upstream_server.local_endpoint().port();

    ts.proxy_server.listen("127.0.0.1", 0);
    ts.proxy_port = ts.proxy_server.local_endpoint().port();

    ts.upstream_server.router()
        .set_http_handler<http::verb::get, http::verb::post, http::verb::put, http::verb::patch,
                          http::verb::delete_>(
            "/echo",
            [](server::request& req, server::response& resp) {
                auto body = as_string(req);
                resp.set_string_content(body, "text/plain");
            });

    ts.upstream_server.router()
        .set_http_handler<http::verb::get>(
            "/status/:code",
            [](server::request& req, server::response& resp) {
                auto code = std::stoi(std::string(req.path_param("code")));
                resp.set_empty_content(static_cast<http::status>(code));
            });

    ts.upstream_server.router()
        .set_http_handler<http::verb::get>(
            "/headers",
            [](server::request& req, server::response& resp) {
                auto xff = req["X-Forwarded-For"];
                resp.set_string_content(std::string(xff), "text/plain");
            });

    ts.upstream_server.router()
        .set_http_handler<http::verb::get>(
            "/resource",
            [](server::request&, server::response& resp) {
                resp.set_string_content(std::string("upstream-resource"), "text/plain");
            });

    ts.proxy_server.set_reverse_proxy(
        "/api/*", ts.upstream_host, ts.upstream_port);

    ts.start();

    SECTION("GET /api/resource → upstream /resource")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/resource"));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp) == "upstream-resource");
    }

    SECTION("GET /api/echo → upstream /echo (empty body)")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/echo"));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp).empty());
    }

    SECTION("POST /api/echo with body → upstream /echo")
    {
        // verify upstream handles POST directly
        auto direct = UNWRAP(ts.upstream_client->post("/echo", std::string_view("direct-post")));
        REQUIRE(direct.result() == http::status::ok);
        REQUIRE(as_string(direct) == "direct-post");

        auto resp = UNWRAP(ts.proxy_client->post("/api/echo", std::string_view("hello-proxy")));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp) == "hello-proxy");
    }

    SECTION("GET /api/status/201 → upstream /status/201")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/201"));
        REQUIRE(resp.result() == http::status::created);
    }

    SECTION("GET /api/status/404 → upstream /status/404")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/404"));
        REQUIRE(resp.result() == http::status::not_found);
    }

    SECTION("X-Forwarded-For is appended")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/headers"));
        REQUIRE(resp.result() == http::status::ok);
        auto body = as_string(resp);
        REQUIRE(body.find("127.0.0.1") != std::string::npos);
    }

    SECTION("path not matching prefix returns 404")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/other/resource"));
        REQUIRE(resp.result() == http::status::not_found);
    }
}
