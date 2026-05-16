#include "httplib/body/json_body.hpp"
#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/html/query_params.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

using namespace std::string_view_literals;
namespace beast = httplib::beast;
namespace http  = httplib::http;
namespace net   = httplib::net;

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

    std::string host() const { return endpoint.address().to_string(); }
    uint16_t port() const { return endpoint.port(); }
};

std::string as_string(const httplib::client::http_client::response& resp)
{
    return resp.body().as<httplib::body::string_body>();
}

const boost::json::value& as_json(const httplib::client::http_client::response& resp)
{
    return resp.body().as<httplib::body::json_body>();
}

#define UNWRAP(result)              \
    [&](auto&& r) {                 \
        REQUIRE(r.has_value());     \
        return std::move(r).value(); \
    }(result)

httplib::html::query_params params(std::initializer_list<std::pair<std::string, std::string>> vals)
{
    httplib::html::query_params out;
    for (const auto& [key, val] : vals)
        out.add(key, val);
    return out;
}

httplib::http::fields headers(std::initializer_list<std::pair<httplib::http::field, std::string>> vals)
{
    httplib::http::fields out;
    for (const auto& [key, val] : vals)
        out.set(key, val);
    return out;
}

void set_text(httplib::server::response& resp,
              std::string_view body,
              httplib::http::status status = httplib::http::status::ok)
{
    resp.set_string_content(body, "text/plain"sv, status);
}

auto base_headers()
{
    return headers({{httplib::http::field::user_agent, "httplib-test"}});
}

auto query_q1() { return params({{"q", "1"}}); }

} // namespace

TEST_CASE("HTTP GET returns correct body and status", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/method/get",
        [](httplib::server::request& req, httplib::server::response& resp) {
            REQUIRE(req.method() == http::verb::get);
            REQUIRE(req.query_params().at("q") == "1");
            set_text(resp, "get-ok"sv);
        });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/method/get", query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "get-ok");
}

TEST_CASE("HTTP HEAD returns correct headers and no body", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::head>(
        "/method/head",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set("X-Head-Test", "head-ok");
            set_text(resp, "head-body-is-not-sent"sv);
        });
    ts.start();

    auto resp = UNWRAP(ts.client->head("/method/head", query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp[http::field::content_type] == "text/plain");
    REQUIRE(resp["X-Head-Test"] == "head-ok");
}

TEST_CASE("HTTP POST with string body", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::post>(
        "/method/post",
        [](httplib::server::request& req, httplib::server::response& resp) {
            REQUIRE(req.body().as<httplib::body::string_body>() == "post-body");
            set_text(resp, "post-ok"sv);
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/method/post", "post-body"sv, query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "post-ok");
}

TEST_CASE("HTTP POST with JSON body", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::post>(
        "/method/post-json",
        [](httplib::server::request& req, httplib::server::response& resp) {
            const auto& obj = req.body().as<httplib::body::json_body>().as_object();
            REQUIRE(obj.at("name").as_string() == "client");
            resp.set_json_content({{"ok", true}, {"echo", obj.at("name")}});
        });
    ts.start();

    auto resp =
        UNWRAP(ts.client->post("/method/post-json", {{"name", "client"}}, query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_json(resp).as_object().at("ok").as_bool());
    REQUIRE(as_json(resp).as_object().at("echo").as_string() == "client");
}

TEST_CASE("HTTP PUT no body", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::put>(
        "/method/put-empty",
        [](httplib::server::request&, httplib::server::response& resp) {
            set_text(resp, "put-empty-ok"sv);
        });
    ts.start();

    auto resp = UNWRAP(ts.client->put("/method/put-empty", query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "put-empty-ok");
}

TEST_CASE("HTTP PUT with string body", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::put>(
        "/method/put",
        [](httplib::server::request& req, httplib::server::response& resp) {
            REQUIRE(req.body().as<httplib::body::string_body>() == "put-body");
            set_text(resp, "put-ok"sv);
        });
    ts.start();

    auto resp = UNWRAP(ts.client->put("/method/put", "put-body"sv, query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "put-ok");
}

TEST_CASE("HTTP PUT with JSON body", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::put>(
        "/method/put-json",
        [](httplib::server::request& req, httplib::server::response& resp) {
            const auto& obj = req.body().as<httplib::body::json_body>().as_object();
            REQUIRE(obj.at("name").as_string() == "put-json");
            resp.set_json_content({{"ok", true}, {"method", "put"}});
        });
    ts.start();

    auto resp = UNWRAP(
        ts.client->put("/method/put-json", {{"name", "put-json"}}, query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_json(resp).as_object().at("method").as_string() == "put");
}

TEST_CASE("HTTP PATCH with string body", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::patch>(
        "/method/patch",
        [](httplib::server::request& req, httplib::server::response& resp) {
            REQUIRE(req.body().as<httplib::body::string_body>() == "patch-body");
            set_text(resp, "patch-ok"sv);
        });
    ts.start();

    auto resp =
        UNWRAP(ts.client->patch("/method/patch", "patch-body"sv, query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "patch-ok");
}

TEST_CASE("HTTP PATCH with JSON body", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::patch>(
        "/method/patch-json",
        [](httplib::server::request& req, httplib::server::response& resp) {
            const auto& obj = req.body().as<httplib::body::json_body>().as_object();
            REQUIRE(obj.at("name").as_string() == "patch-json");
            resp.set_json_content({{"ok", true}, {"method", "patch"}});
        });
    ts.start();

    auto resp = UNWRAP(ts.client->patch(
        "/method/patch-json", {{"name", "patch-json"}}, query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_json(resp).as_object().at("method").as_string() == "patch");
}

TEST_CASE("HTTP DELETE", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::delete_>(
        "/method/delete",
        [](httplib::server::request&, httplib::server::response& resp) {
            set_text(resp, "delete-ok"sv);
        });
    ts.start();

    auto resp = UNWRAP(ts.client->del("/method/delete", query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "delete-ok");
}

TEST_CASE("HTTP OPTIONS with Allow header", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::options>(
        "/method/options",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set("Allow", "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
            set_text(resp, "options-ok"sv);
        });
    ts.start();

    auto resp = UNWRAP(ts.client->options("/method/options", query_q1(), base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp[http::field::allow] == "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
    REQUIRE(as_string(resp) == "options-ok");
}

TEST_CASE("send_request generic method", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::post>(
        "/method/send-request",
        [](httplib::server::request& req, httplib::server::response& resp) {
            REQUIRE(req.body().as<httplib::body::string_body>() == "generic-body");
            set_text(resp, "send-request-ok"sv);
        });
    ts.start();

    auto resp = UNWRAP(ts.client->send_request(http::verb::post, "/method/send-request",
                                                "generic-body"sv, base_headers()));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "send-request-ok");
}

TEST_CASE("Not found handler returns 404", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_not_found_handler(
        [](httplib::server::request& req, httplib::server::response& resp) {
            REQUIRE(req.path() == "/missing");
            set_text(resp, "not-found"sv, http::status::not_found);
        });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/missing"));
    REQUIRE(resp.result() == http::status::not_found);
    REQUIRE(as_string(resp) == "not-found");
}

TEST_CASE("Client respects user-agent header", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/agent",
        [](httplib::server::request& req, httplib::server::response& resp) {
            REQUIRE(req.base()[http::field::user_agent] == "custom-agent");
            set_text(resp, "ok"sv);
        });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/agent", {},
                                       headers({{http::field::user_agent, "custom-agent"}})));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Multiple query parameters", "[http-methods]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/multi-query",
        [](httplib::server::request& req, httplib::server::response& resp) {
            REQUIRE(req.query_params().at("a") == "1");
            REQUIRE(req.query_params().at("b") == "hello");
            REQUIRE(req.query_params().at("c") == "true");
            set_text(resp, "ok"sv);
        });
    ts.start();

    auto query = params({{"a", "1"}, {"b", "hello"}, {"c", "true"}});
    auto resp  = UNWRAP(ts.client->get("/multi-query", query));
    REQUIRE(resp.result() == http::status::ok);
}
