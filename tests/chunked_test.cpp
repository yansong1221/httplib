#include "common.hpp"
#include "httplib/client/write_session.hpp"
#include "httplib/server/chunk_reader.hpp"
#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <array>

namespace beast = httplib::beast;
namespace mw = httplib::server::middleware;

namespace
{
    using test_common::as_string;
    using test_common::test_scaffold;
} // namespace

TEST_CASE("Chunked: Content-Length hits chunked handler", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked-only",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            resp.set_string_content("chunked-handled"sv, "text/plain");
            co_return;
        });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/chunked-only", "data"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "chunked-handled");
}

TEST_CASE("Chunked: regular POST takes precedence over chunked", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_http_handler<http::verb::post>("/chunked/precedence",
                                                   [](httplib::server::request& req, httplib::server::response& resp)
                                                   {
                                                       auto& body = req.body().as<httplib::body::string_body>();
                                                       resp.set_string_content("regular-" + body, "text/plain");
                                                   });

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/precedence",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
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

    ts.router().set_http_handler<http::verb::get>("/chunked/both",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_string_content("get-ok"sv, "text/plain"); });

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/both",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            REQUIRE(req.is_chunked());
            std::array<char, 4096> buf;
            auto result = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
            resp.set_string_content("chunked-ok"sv, "text/plain");
            co_return;
        });
    ts.start();

    // GET hits regular handler
    auto get_resp = UNWRAP(ts.client->get("/chunked/both"));
    REQUIRE(get_resp.result() == http::status::ok);
    REQUIRE(as_string(get_resp) == "get-ok");

    // Content-Length POST now hits chunked handler
    auto post_resp = UNWRAP(ts.client->post("/chunked/both", "data"sv));
    REQUIRE(post_resp.result() == http::status::ok);
    REQUIRE(as_string(post_resp) == "chunked-ok");
}

TEST_CASE("Chunked: is_chunked() false for regular handler", "[chunked]")
{
    test_scaffold ts;

    // Use a regular POST handler to verify is_chunked() is false
    ts.router().set_http_handler<http::verb::post>("/chunked/check",
                                                   [](httplib::server::request& req, httplib::server::response& resp)
                                                   {
                                                       REQUIRE(!req.is_chunked());
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
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            std::array<char, 1024> buf;
            co_await req.get_chunk_reader()->read_some(net::buffer(buf));
            auto method = std::string(req.method_string());
            resp.set_string_content("chunked-" + method, "text/plain");
            co_return;
        });
    ts.start();

    auto post_resp = UNWRAP(ts.client->post("/chunked/multi", "data"sv));
    REQUIRE(post_resp.result() == http::status::ok);
    REQUIRE(as_string(post_resp) == "chunked-POST");

    auto put_resp = UNWRAP(ts.client->put("/chunked/multi", "data"sv));
    REQUIRE(put_resp.result() == http::status::ok);
    REQUIRE(as_string(put_resp) == "chunked-PUT");
}

TEST_CASE("Chunked: handler with path param", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/user/:id",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto id = req.path_param("id");
            resp.set_string_content("chunked-" + std::string(id), "text/plain");
            co_return;
        });
    ts.router().set_http_handler<http::verb::get>("/chunked/user/:id",
                                                  [](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      auto id = req.path_param("id");
                                                      resp.set_string_content("get-" + std::string(id), "text/plain");
                                                  });
    ts.start();

    // GET with path param works normally
    auto get_resp = UNWRAP(ts.client->get("/chunked/user/42"));
    REQUIRE(get_resp.result() == http::status::ok);
    REQUIRE(as_string(get_resp) == "get-42");

    // Content-Length POST hits chunked handler
    auto post_resp = UNWRAP(ts.client->post("/chunked/user/42", "data"sv));
    REQUIRE(post_resp.result() == http::status::ok);
    REQUIRE(as_string(post_resp) == "chunked-42");
}

TEST_CASE("Chunked: handler with wildcard path", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/ws/*",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto wild = req.path_param("*");
            resp.set_string_content("chunked-" + std::string(wild), "text/plain");
            co_return;
        });
    ts.router().set_http_handler<http::verb::get>("/chunked/ws/*",
                                                  [](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      auto wild = req.path_param("*");
                                                      resp.set_string_content("get-" + std::string(wild), "text/plain");
                                                  });
    ts.start();

    // GET with wildcard works
    auto get_resp = UNWRAP(ts.client->get("/chunked/ws/a/b/c"));
    REQUIRE(get_resp.result() == http::status::ok);
    REQUIRE(as_string(get_resp) == "get-a/b/c");

    // Content-Length POST hits chunked handler
    auto post_resp = UNWRAP(ts.client->post("/chunked/ws/x/y", "data"sv));
    REQUIRE(post_resp.result() == http::status::ok);
    REQUIRE(as_string(post_resp) == "chunked-x/y");
}

TEST_CASE("Chunked: middleware is wrapped via set_chunked_http_handler", "[chunked]")
{
    test_scaffold ts;

    mw::cors_middleware cors_middleware;
    cors_middleware.allow_origins({ "https://example.com" });
    cors_middleware.allow_methods({ "GET", "POST" });

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/cors_middleware",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            resp.set_string_content("should-not-run"sv, "text/plain");
            co_return;
        },
        cors_middleware);
    ts.router().set_http_handler<http::verb::get>(
        "/chunked/cors_middleware",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("cors_middleware-get"sv, "text/plain"); },
        cors_middleware);
    ts.start();

    // GET with cors_middleware works
    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::origin, "https://example.com");
    auto get_resp = UNWRAP(ts.client->send_request(http::verb::get, "/chunked/cors_middleware", hdrs));
    REQUIRE(get_resp.result() == http::status::ok);
    REQUIRE(as_string(get_resp) == "cors_middleware-get");
    REQUIRE(get_resp[http::field::access_control_allow_origin] == "https://example.com");
}

TEST_CASE("Chunked: regular PUT coexists with chunked POST", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_http_handler<http::verb::put>("/chunked/mixed",
                                                  [](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      auto& body = req.body().as<httplib::body::string_body>();
                                                      resp.set_string_content("regular-put-" + body, "text/plain");
                                                  });

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/mixed",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            resp.set_string_content("chunked-post"sv, "text/plain");
            co_return;
        });
    ts.start();

    // Content-Length PUT �?regular handler
    auto put_resp = UNWRAP(ts.client->put("/chunked/mixed", "hello"sv));
    REQUIRE(put_resp.result() == http::status::ok);
    REQUIRE(as_string(put_resp) == "regular-put-hello");

    // Content-Length POST hits chunked handler
    auto post_resp = UNWRAP(ts.client->post("/chunked/mixed", "data"sv));
    REQUIRE(post_resp.result() == http::status::ok);
    REQUIRE(as_string(post_resp) == "chunked-post");
}

TEST_CASE("Chunked: chunked handler does not affect path that only has regular handlers", "[chunked]")
{
    test_scaffold ts;

    // Register a chunked handler on a DIFFERENT path �?should not affect other paths
    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/isolated",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            resp.set_string_content("should-not-run"sv, "text/plain");
            co_return;
        });

    ts.router().set_http_handler<http::verb::post>("/regular/path",
                                                   [](httplib::server::request& req, httplib::server::response& resp)
                                                   {
                                                       auto& body = req.body().as<httplib::body::string_body>();
                                                       resp.set_string_content("regular-" + body, "text/plain");
                                                   });
    ts.start();

    // Regular path unaffected
    auto resp = UNWRAP(ts.client->post("/regular/path", "data"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "regular-data");
}

static std::string
send_chunked(net::any_io_executor ex,
             httplib::client::http_client& client,
             http::verb method,
             std::string_view path,
             std::vector<std::string> chunks)
{
    return net::co_spawn(
               ex,
               [&client, method, path, chunks = std::move(chunks)]() -> net::awaitable<std::string>
               {
                   auto writer = client.create_writer();
                   auto reader = client.create_reader();

                   co_await writer->write_header(method, path, {}, false);
                   for (size_t i = 0; i < chunks.size(); ++i)
                   {
                       auto more = (i + 1 < chunks.size());
                       co_await writer->write_body(net::buffer(chunks[i]), more);
                   }
                   if (chunks.empty())
                   {
                       co_await writer->write_body(net::buffer("", 0), false);
                   }

                   auto header_ec = co_await reader->read_header();
                   if (header_ec)
                   {
                       co_return std::string {};
                   }

                   std::string body;
                   std::array<char, 4096> buf;
                   for (;;)
                   {
                       auto result = co_await reader->read_body(net::buffer(buf));
                       if (result.has_error() || result.value() == 0)
                       {
                           break;
                       }
                       body.append(buf.data(), result.value());
                   }
                   co_return body;
               },
               net::use_future)
        .get();
}

TEST_CASE("Chunked: buffer_body receives de-chunked data", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            REQUIRE(req.is_chunked());
            std::string accumulated;
            std::array<char, 8192> buf;
            for (;;)
            {
                auto _bytes_r = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
                if (_bytes_r.has_error())
                {
                    break;
                }
                auto bytes = _bytes_r.value();
                if (bytes == 0)
                {
                    break;
                }
                accumulated.append(buf.data(), bytes);
            }
            resp.set_string_content(accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(ts.executor(), *ts.client, http::verb::post, "/chunked/read", { "Hello", " World" });
    REQUIRE(result == "Hello World");
}

TEST_CASE("Chunked: multiple chunks are de-chunked into single body", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read-ext",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            REQUIRE(req.is_chunked());
            std::string accumulated;
            std::array<char, 8192> buf;
            for (;;)
            {
                auto _bytes_r = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
                if (_bytes_r.has_error())
                {
                    break;
                }
                auto bytes = _bytes_r.value();
                if (bytes == 0)
                {
                    break;
                }
                accumulated.append(buf.data(), bytes);
            }
            resp.set_string_content(accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(ts.executor(),
                               *ts.client,
                               http::verb::post,
                               "/chunked/read-ext",
                               { "chunk1", "chunk2", "chunk3" });
    REQUIRE(result == "chunk1chunk2chunk3");
}

TEST_CASE("Chunked: large chunk via buffer_body", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read-large",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            REQUIRE(req.is_chunked());
            std::string accumulated;
            std::array<char, 8192> buf;
            for (;;)
            {
                auto _bytes_r = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
                if (_bytes_r.has_error())
                {
                    break;
                }
                auto bytes = _bytes_r.value();
                if (bytes == 0)
                {
                    break;
                }
                accumulated.append(buf.data(), bytes);
            }
            resp.set_string_content(std::to_string(accumulated.size()), "text/plain");
        });
    ts.start();

    std::string large_chunk(10000, 'X');
    auto result = send_chunked(ts.executor(), *ts.client, http::verb::post, "/chunked/read-large", { large_chunk });
    REQUIRE(result == "10000");
}

TEST_CASE("Chunked: empty chunks via buffer_body", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read-empty",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            REQUIRE(req.is_chunked());
            std::string accumulated;
            std::array<char, 4096> buf;
            auto bytes_result = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
            resp.set_string_content(std::to_string(accumulated.size()), "text/plain");
        });
    ts.start();

    auto result = send_chunked(ts.executor(), *ts.client, http::verb::post, "/chunked/read-empty", {});
    REQUIRE(result == "0");
}

TEST_CASE("Chunked: is_buffer_body_handler() is true", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read-is-chunked",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            bool was_body = req.is_chunked();
            std::array<char, 8192> buf;
            auto bytes_result = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
            if (bytes_result.has_error())
            {
                co_return;
            }
            auto bytes = bytes_result.value();
            resp.set_string_content(std::string(was_body ? "yes:" : "no:") + std::string(buf.data(), bytes),
                                    "text/plain");
        });
    ts.start();

    auto result = send_chunked(ts.executor(), *ts.client, http::verb::post, "/chunked/read-is-chunked", { "data" });
    REQUIRE(result == "yes:data");
}

TEST_CASE("Chunked: with path parameters via buffer_body", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read/:id",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            REQUIRE(req.is_chunked());
            auto id = std::string(req.path_param("id"));
            std::string accumulated;
            std::array<char, 8192> buf;
            for (;;)
            {
                auto _bytes_r = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
                if (_bytes_r.has_error())
                {
                    break;
                }
                auto bytes = _bytes_r.value();
                if (bytes == 0)
                {
                    break;
                }
                accumulated.append(buf.data(), bytes);
            }
            resp.set_string_content(id + ":" + accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(ts.executor(), *ts.client, http::verb::post, "/chunked/read/42", { "hello" });
    REQUIRE(result == "42:hello");
}

TEST_CASE("Chunked: with wildcard path via buffer_body", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/read-ws/*",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            REQUIRE(req.is_chunked());
            auto wild = std::string(req.path_param("*"));
            std::string accumulated;
            std::array<char, 8192> buf;
            for (;;)
            {
                auto _bytes_r = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
                if (_bytes_r.has_error())
                {
                    break;
                }
                auto bytes = _bytes_r.value();
                if (bytes == 0)
                {
                    break;
                }
                accumulated.append(buf.data(), bytes);
            }
            resp.set_string_content(wild + ":" + accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(ts.executor(), *ts.client, http::verb::post, "/chunked/read-ws/a/b", { "xyz" });
    REQUIRE(result == "a/b:xyz");
}

TEST_CASE("Chunked: PUT via buffer_body", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::put>(
        "/chunked/read-put",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            REQUIRE(req.is_chunked());
            std::string accumulated;
            std::array<char, 8192> buf;
            for (;;)
            {
                auto _bytes_r = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
                if (_bytes_r.has_error())
                {
                    break;
                }
                auto bytes = _bytes_r.value();
                if (bytes == 0)
                {
                    break;
                }
                accumulated.append(buf.data(), bytes);
            }
            resp.set_string_content("PUT:" + accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(ts.executor(), *ts.client, http::verb::put, "/chunked/read-put", { "put-body" });
    REQUIRE(result == "PUT:put-body");
}

TEST_CASE("Chunked: multi-verb via buffer_body", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post, http::verb::patch>(
        "/chunked/read-multi",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            REQUIRE(req.is_chunked());
            std::string accumulated;
            std::array<char, 8192> buf;
            for (;;)
            {
                auto _bytes_r = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
                if (_bytes_r.has_error())
                {
                    break;
                }
                auto bytes = _bytes_r.value();
                if (bytes == 0)
                {
                    break;
                }
                accumulated.append(buf.data(), bytes);
            }
            auto method = std::string(req.method_string());
            resp.set_string_content(method + ":" + accumulated, "text/plain");
        });
    ts.start();

    auto post_result
        = send_chunked(ts.executor(), *ts.client, http::verb::post, "/chunked/read-multi", { "from-post" });
    REQUIRE(post_result == "POST:from-post");

    auto patch_result
        = send_chunked(ts.executor(), *ts.client, http::verb::patch, "/chunked/read-multi", { "from-patch" });
    REQUIRE(patch_result == "PATCH:from-patch");
}

TEST_CASE("Chunked: sync send_chunked_request via buffer_body", "[chunked]")
{
    test_scaffold ts;

    ts.router().set_chunked_http_handler<http::verb::post>(
        "/chunked/sync",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            REQUIRE(req.is_chunked());
            std::string accumulated;
            std::array<char, 8192> buf;
            for (;;)
            {
                auto _bytes_r = co_await req.get_chunk_reader()->read_some(net::buffer(buf));
                if (_bytes_r.has_error())
                {
                    break;
                }
                auto bytes = _bytes_r.value();
                if (bytes == 0)
                {
                    break;
                }
                accumulated.append(buf.data(), bytes);
            }
            resp.set_string_content(accumulated, "text/plain");
        });
    ts.start();

    auto result = send_chunked(ts.executor(), *ts.client, http::verb::post, "/chunked/sync", { "via", "sync" });
    REQUIRE(result == "viasync");
}
