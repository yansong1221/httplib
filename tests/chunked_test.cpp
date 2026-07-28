#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
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

struct test_scaffold {
    net::io_context ioc;
    httplib::server::http_server server;
    httplib::tcp::endpoint endpoint;
    std::thread thread;
    std::unique_ptr<httplib::client::http_client> client;
    bool started_ = false;

    test_scaffold() : server(ioc)
    {
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        server.set_logger(std::make_shared<spdlog::logger>("httplib.tests", null_sink));
    }

    ~test_scaffold()
    {
        if (started_) {
            server.stop().wait();
            ioc.stop();
            if (thread.joinable())
                thread.join();
        }
    }

    void start()
    {
        server.listen("127.0.0.1", 0);
        endpoint = server.local_endpoint();
        server.run();
        thread = std::thread([this] { ioc.run(); });
        started_ = true;

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

#define UNWRAP(result)              \
    [&](auto&& r) {                 \
        REQUIRE(r.has_value());     \
        return std::move(r).value(); \
    }(result)

} // namespace

TEST_CASE("Chunked: Content-Length does NOT hit chunked handler", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked-only",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void> {
            resp.set_string_content("should-not-run"sv, "text/plain");
            co_return;
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/chunked-only", "data"sv));
    REQUIRE(resp.result() == http::status::not_found);
}

TEST_CASE("Chunked: regular POST takes precedence over chunked", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_http_handler<http::verb::post>(
        "/chunked/precedence",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto& body = req.body().as<httplib::body::string_body>();
            resp.set_string_content("regular-" + body, "text/plain");
        });

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/precedence",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void> {
            resp.set_string_content("should-not-run"sv, "text/plain");
            co_return;
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/chunked/precedence", "data"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "regular-data");
}

TEST_CASE("Chunked: GET coexists with chunked POST, returns 405 for Content-Length POST", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_http_handler<http::verb::get>(
        "/chunked/both",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("get-ok"sv, "text/plain");
        });

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/both",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void> {
            resp.set_string_content("should-not-run"sv, "text/plain");
            co_return;
        });
    ts.start();

    // GET hits regular handler
    auto get_resp = UNWRAP(ts.client->get("/chunked/both"));
    REQUIRE(get_resp.result() == http::status::ok);
    REQUIRE(as_string(get_resp) == "get-ok");

    // Content-Length POST → 405 (path exists but POST via chunked only)
    auto post_resp = UNWRAP(ts.client->post("/chunked/both", "data"sv));
    REQUIRE(post_resp.result() == http::status::method_not_allowed);
    REQUIRE(post_resp[http::field::allow] == "GET");
}

TEST_CASE("Chunked: is_chunked_handler() false for Content-Length request", "[chunked]")
{
    test_scaffold ts;

    // Use a regular POST handler to verify is_chunked_handler() is false
    ts.router().set_http_handler<http::verb::post>(
        "/chunked/check",
        [](httplib::server::request& req, httplib::server::response& resp) {
            REQUIRE(!req.is_chunked_handler());
            resp.set_string_content("not-chunked"sv, "text/plain");
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/chunked/check", "data"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "not-chunked");
}

TEST_CASE("Chunked: read_chunk() returns empty when not set up", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_http_handler<http::verb::post>(
        "/chunked/noctx",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(!req.is_chunked_handler());
            auto chunk = co_await req.read_chunk();
            REQUIRE(chunk.empty());
            resp.set_string_content("ok"sv, "text/plain");
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/chunked/noctx", "data"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "ok");
}

TEST_CASE("Chunked: multi-verb chunked handler registration", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post, http::verb::put>(
        "/chunked/multi",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void> {
            resp.set_string_content("should-not-run"sv, "text/plain");
            co_return;
        });
    ts.router().set_http_handler<http::verb::get>(
        "/chunked/multi",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("get-ok"sv, "text/plain");
        });
    ts.start();

    // Content-Length POST → 405 (GET exists on path, chunked POST not applicable)
    auto post_resp = UNWRAP(ts.client->post("/chunked/multi", "data"sv));
    REQUIRE(post_resp.result() == http::status::method_not_allowed);
    REQUIRE(post_resp[http::field::allow] == "GET");

    // Content-Length PUT → 405
    auto put_resp = UNWRAP(ts.client->put("/chunked/multi", "data"sv));
    REQUIRE(put_resp.result() == http::status::method_not_allowed);

    // GET still works via regular handler
    auto get_resp = UNWRAP(ts.client->get("/chunked/multi"));
    REQUIRE(get_resp.result() == http::status::ok);
    REQUIRE(as_string(get_resp) == "get-ok");
}

TEST_CASE("Chunked: handler with path param", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/user/:id",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            auto id = req.path_param("id");
            resp.set_string_content("chunked-" + std::string(id) + "-should-not-run",
                                    "text/plain");
            co_return;
        });
    ts.router().set_http_handler<http::verb::get>(
        "/chunked/user/:id",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto id = req.path_param("id");
            resp.set_string_content("get-" + std::string(id), "text/plain");
        });
    ts.start();

    // GET with path param works normally
    auto get_resp = UNWRAP(ts.client->get("/chunked/user/42"));
    REQUIRE(get_resp.result() == http::status::ok);
    REQUIRE(as_string(get_resp) == "get-42");

    // Content-Length POST → 405 (GET exists on path)
    auto post_resp = UNWRAP(ts.client->post("/chunked/user/42", "data"sv));
    REQUIRE(post_resp.result() == http::status::method_not_allowed);
    REQUIRE(post_resp[http::field::allow] == "GET");
}

TEST_CASE("Chunked: handler with wildcard path", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/ws/*",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            auto wild = req.path_param("*");
            resp.set_string_content("chunked-" + std::string(wild) + "-should-not-run",
                                    "text/plain");
            co_return;
        });
    ts.router().set_http_handler<http::verb::get>(
        "/chunked/ws/*",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto wild = req.path_param("*");
            resp.set_string_content("get-" + std::string(wild), "text/plain");
        });
    ts.start();

    // GET with wildcard works
    auto get_resp = UNWRAP(ts.client->get("/chunked/ws/a/b/c"));
    REQUIRE(get_resp.result() == http::status::ok);
    REQUIRE(as_string(get_resp) == "get-a/b/c");

    // Content-Length POST → 405 (GET exists on path)
    auto post_resp = UNWRAP(ts.client->post("/chunked/ws/x/y", "data"sv));
    REQUIRE(post_resp.result() == http::status::method_not_allowed);
    REQUIRE(post_resp[http::field::allow] == "GET");
}

TEST_CASE("Chunked: middleware is wrapped via set_chunked_http_handler", "[chunked]")
{
    test_scaffold ts;

    mw::cors_middleware cors;
    cors.allow_origins({"https://example.com"});
    cors.allow_methods({"GET", "POST"});

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/cors",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void> {
            resp.set_string_content("should-not-run"sv, "text/plain");
            co_return;
        },
        cors);
    ts.router().set_http_handler<http::verb::get>(
        "/chunked/cors",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_string_content("cors-get"sv, "text/plain");
        },
        cors);
    ts.start();

    // GET with CORS works
    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::origin, "https://example.com");
    auto get_resp = UNWRAP(ts.client->send_request(
        http::verb::get, "/chunked/cors", hdrs));
    REQUIRE(get_resp.result() == http::status::ok);
    REQUIRE(as_string(get_resp) == "cors-get");
    REQUIRE(get_resp[http::field::access_control_allow_origin] == "https://example.com");
}

TEST_CASE("Chunked: regular PUT coexists with chunked POST", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_http_handler<http::verb::put>(
        "/chunked/mixed",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto& body = req.body().as<httplib::body::string_body>();
            resp.set_string_content("regular-put-" + body, "text/plain");
        });

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/mixed",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void> {
            resp.set_string_content("should-not-run"sv, "text/plain");
            co_return;
        });
    ts.start();

    // Content-Length PUT → regular handler
    auto put_resp = UNWRAP(ts.client->put("/chunked/mixed", "hello"sv));
    REQUIRE(put_resp.result() == http::status::ok);
    REQUIRE(as_string(put_resp) == "regular-put-hello");

    // Content-Length POST → 405 (PUT exists on path, chunked POST not applicable)
    auto post_resp = UNWRAP(ts.client->post("/chunked/mixed", "data"sv));
    REQUIRE(post_resp.result() == http::status::method_not_allowed);
    REQUIRE(post_resp[http::field::allow] == "PUT");
}

TEST_CASE("Chunked: chunked handler does not affect path that only has regular handlers",
          "[chunked]")
{
    test_scaffold ts;

    // Register a chunked handler on a DIFFERENT path — should not affect other paths
    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/isolated",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void> {
            resp.set_string_content("should-not-run"sv, "text/plain");
            co_return;
        });

    ts.router().set_http_handler<http::verb::post>(
        "/regular/path",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto& body = req.body().as<httplib::body::string_body>();
            resp.set_string_content("regular-" + body, "text/plain");
        });
    ts.start();

    // Regular path unaffected
    auto resp = UNWRAP(ts.client->post("/regular/path", "data"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "regular-data");
}
