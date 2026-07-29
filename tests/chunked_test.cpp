#include "httplib/body/string_body.hpp"
#include "httplib/client/chunk_writer.hpp"
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
namespace http  = httplib::http;
namespace net   = httplib::net;
namespace beast = httplib::beast;
namespace mw    = httplib::server::middleware;

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


static auto chunk_vec_generator(std::vector<std::string> chunks)
{
    return [chunks = std::move(chunks),
            idx = std::make_shared<size_t>(0)](httplib::client::chunk_writer& writer) -> net::awaitable<void> {
        for (const auto& chunk : chunks)
            co_await writer.write_chunk(chunk);
    };
}

static std::string
send_chunked(httplib::client::http_client& client,
             http::verb method,
             std::string_view path,
             std::vector<std::string> chunks)
{
    auto gen  = chunk_vec_generator(std::move(chunks));
    auto resp = UNWRAP(client.send_chunked_request(method, path, std::move(gen)));
    return resp.body().as<httplib::body::string_body>();
}

TEST_CASE("Chunked: read_chunk() receives all chunks", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_chunked_handler());
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.get_chunk_reader().read_chunk();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content(accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(
        *ts.client, http::verb::post, "/chunked/read", {"Hello", " World"});
    REQUIRE(result == "Hello World");
}

TEST_CASE("Chunked: read_chunk() with multiple non-empty chunks", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read-ext",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_chunked_handler());
            int chunk_count = 0;
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.get_chunk_reader().read_chunk();
                if (chunk.empty())
                    break;
                ++chunk_count;
                accumulated += std::string(chunk) + "|";
            }
            resp.set_string_content(std::to_string(chunk_count) + ":" + accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(
        *ts.client, http::verb::post, "/chunked/read-ext", {"chunk1", "chunk2", "chunk3"});
    REQUIRE(result == "3:chunk1|chunk2|chunk3|");
}

TEST_CASE("Chunked: read_chunk() with single large chunk", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read-large",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_chunked_handler());
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.get_chunk_reader().read_chunk();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content(std::to_string(accumulated.size()), "text/plain");
        });
    ts.start();

    std::string large_chunk(10000, 'X');
    auto result = send_chunked(
        *ts.client, http::verb::post, "/chunked/read-large", {large_chunk});
    REQUIRE(result == "10000");
}

TEST_CASE("Chunked: read_chunk() returns empty for zero-chunk request", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read-empty",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_chunked_handler());
            int count = 0;
            for (;;) {
                auto chunk = co_await req.get_chunk_reader().read_chunk();
                if (chunk.empty())
                    break;
                ++count;
            }
            resp.set_string_content(std::to_string(count), "text/plain");
        });
    ts.start();

    auto result = send_chunked(
        *ts.client, http::verb::post, "/chunked/read-empty", {});
    REQUIRE(result == "0");
}

TEST_CASE("Chunked: read_chunk() respects is_chunked_handler() = true", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read-is-chunked",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            bool was_chunked = req.is_chunked_handler();
            auto chunk       = co_await req.get_chunk_reader().read_chunk();
            resp.set_string_content(std::string(was_chunked ? "yes:" : "no:") +
                                        std::string(chunk),
                                    "text/plain");
        });
    ts.start();

    auto result = send_chunked(
        *ts.client, http::verb::post, "/chunked/read-is-chunked", {"data"});
    REQUIRE(result == "yes:data");
}

TEST_CASE("Chunked: read_chunk() with path parameters", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read/:id",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_chunked_handler());
            auto id = std::string(req.path_param("id"));
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.get_chunk_reader().read_chunk();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content(id + ":" + accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(
        *ts.client, http::verb::post, "/chunked/read/42", {"hello"});
    REQUIRE(result == "42:hello");
}

TEST_CASE("Chunked: read_chunk() with wildcard path", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read-ws/*",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_chunked_handler());
            auto wild = std::string(req.path_param("*"));
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.get_chunk_reader().read_chunk();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content(wild + ":" + accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(
        *ts.client, http::verb::post, "/chunked/read-ws/a/b", {"xyz"});
    REQUIRE(result == "a/b:xyz");
}

TEST_CASE("Chunked: read_chunk() via PUT chunked handler", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::put>(
        "/chunked/read-put",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_chunked_handler());
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.get_chunk_reader().read_chunk();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content("PUT:" + accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(
        *ts.client, http::verb::put, "/chunked/read-put", {"put-body"});
    REQUIRE(result == "PUT:put-body");
}

TEST_CASE("Chunked: read_chunk() via multi-verb chunked handler", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post, http::verb::patch>(
        "/chunked/read-multi",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_chunked_handler());
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.get_chunk_reader().read_chunk();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            auto method = std::string(req.method_string());
            resp.set_string_content(method + ":" + accumulated, "text/plain");
        });
    ts.start();

    auto post_result = send_chunked(
        *ts.client, http::verb::post, "/chunked/read-multi", {"from-post"});
    REQUIRE(post_result == "POST:from-post");

    auto patch_result = send_chunked(
        *ts.client, http::verb::patch, "/chunked/read-multi", {"from-patch"});
    REQUIRE(patch_result == "PATCH:from-patch");
}

TEST_CASE("Chunked: async_send_chunked_request via send_chunked_request", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/sync",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_chunked_handler());
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.get_chunk_reader().read_chunk();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content(accumulated, "text/plain");
        });
    ts.start();

    auto gen = chunk_vec_generator({"via", "sync"});
    auto resp = UNWRAP(ts.client->send_chunked_request(
        http::verb::post, "/chunked/sync", std::move(gen)));
    REQUIRE(resp.body().as<httplib::body::string_body>() == "viasync");
}

// =============================================================================
// buffer_body handler tests
// =============================================================================

TEST_CASE("BufferBody: read_buffer_body_some() receives body incrementally", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_buffer_body_http_handler<http::verb::post>(
        "/buffer/read",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_buffer_body_handler());
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.read_buffer_body_some();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content(accumulated, "text/plain");
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/buffer/read", "Hello World"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "Hello World");
}

TEST_CASE("BufferBody: read_buffer_body_some() with empty body", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_buffer_body_http_handler<http::verb::post>(
        "/buffer/empty",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_buffer_body_handler());
            int count = 0;
            for (;;) {
                auto chunk = co_await req.read_buffer_body_some();
                if (chunk.empty())
                    break;
                ++count;
            }
            resp.set_string_content(std::to_string(count), "text/plain");
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/buffer/empty", ""sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "0");
}

TEST_CASE("BufferBody: read_buffer_body_some() with large body", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_buffer_body_http_handler<http::verb::post>(
        "/buffer/large",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_buffer_body_handler());
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.read_buffer_body_some();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content(std::to_string(accumulated.size()), "text/plain");
        });
    ts.start();

    std::string large_body(10000, 'X');
    auto resp = UNWRAP(ts.client->post("/buffer/large", large_body));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "10000");
}

TEST_CASE("BufferBody: is_buffer_body_handler() is true", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_buffer_body_http_handler<http::verb::post>(
        "/buffer/is-handler",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            bool was_buffer = req.is_buffer_body_handler();
            auto chunk      = co_await req.read_buffer_body_some();
            resp.set_string_content(std::string(was_buffer ? "yes:" : "no:") +
                                        std::string(chunk),
                                    "text/plain");
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/buffer/is-handler", "data"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "yes:data");
}

TEST_CASE("BufferBody: read_buffer_body_some() returns empty when not set up", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_http_handler<http::verb::post>(
        "/buffer/noctx",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(!req.is_buffer_body_handler());
            auto chunk = co_await req.read_buffer_body_some();
            REQUIRE(chunk.empty());
            resp.set_string_content("ok"sv, "text/plain");
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/buffer/noctx", "data"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "ok");
}

TEST_CASE("BufferBody: with path parameters", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_buffer_body_http_handler<http::verb::post>(
        "/buffer/user/:id",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_buffer_body_handler());
            auto id = std::string(req.path_param("id"));
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.read_buffer_body_some();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content(id + ":" + accumulated, "text/plain");
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/buffer/user/99", "hello"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "99:hello");
}

TEST_CASE("BufferBody: with wildcard path", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_buffer_body_http_handler<http::verb::post>(
        "/buffer/ws/*",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_buffer_body_handler());
            auto wild = std::string(req.path_param("*"));
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.read_buffer_body_some();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content(wild + ":" + accumulated, "text/plain");
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/buffer/ws/a/b/c", "xyz"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "a/b/c:xyz");
}

TEST_CASE("BufferBody: multi-verb registration", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_buffer_body_http_handler<http::verb::post, http::verb::patch>(
        "/buffer/multi",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_buffer_body_handler());
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.read_buffer_body_some();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            auto method = std::string(req.method_string());
            resp.set_string_content(method + ":" + accumulated, "text/plain");
        });
    ts.start();

    auto post_resp = UNWRAP(ts.client->post("/buffer/multi", "post-body"sv));
    REQUIRE(post_resp.result() == http::status::ok);
    REQUIRE(as_string(post_resp) == "POST:post-body");

    auto patch_resp = UNWRAP(ts.client->patch("/buffer/multi", "patch-body"sv));
    REQUIRE(patch_resp.result() == http::status::ok);
    REQUIRE(as_string(patch_resp) == "PATCH:patch-body");
}

TEST_CASE("BufferBody: regular handler takes precedence", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_http_handler<http::verb::post>(
        "/buffer/precedence",
        [](httplib::server::request& req, httplib::server::response& resp) {
            auto& body = req.body().as<httplib::body::string_body>();
            resp.set_string_content("regular-" + body, "text/plain");
        });

    ts.router().set_buffer_body_http_handler<http::verb::post>(
        "/buffer/precedence",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void> {
            resp.set_string_content("should-not-run"sv, "text/plain");
            co_return;
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/buffer/precedence", "data"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "regular-data");
}

TEST_CASE("BufferBody: Content-Length POST does NOT hit chunked handler on same path",
          "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/buffer/vs-chunked",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void> {
            resp.set_string_content("chunked"sv, "text/plain");
            co_return;
        });

    ts.router().set_buffer_body_http_handler<http::verb::post>(
        "/buffer/vs-chunked",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_buffer_body_handler());
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.read_buffer_body_some();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content("buffer-" + accumulated, "text/plain");
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/buffer/vs-chunked", "body"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "buffer-body");
}

TEST_CASE("BufferBody: via PUT method", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_buffer_body_http_handler<http::verb::put>(
        "/buffer/put",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void> {
            REQUIRE(req.is_buffer_body_handler());
            std::string accumulated;
            for (;;) {
                auto chunk = co_await req.read_buffer_body_some();
                if (chunk.empty())
                    break;
                accumulated.append(chunk);
            }
            resp.set_string_content("PUT:" + accumulated, "text/plain");
        });
    ts.start();

    auto resp = UNWRAP(ts.client->put("/buffer/put", "put-body"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "PUT:put-body");
}

TEST_CASE("BufferBody: handler does not affect other paths", "[buffer_body]")
{
    test_scaffold ts;

    ts.router().set_buffer_body_http_handler<http::verb::post>(
        "/buffer/isolated",
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

    auto resp = UNWRAP(ts.client->post("/regular/path", "data"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "regular-data");
}
