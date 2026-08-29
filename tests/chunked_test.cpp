#include "body/string_body.hpp"
#include "common.hpp"
#include "httplib/client/lazy_request.hpp"
#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace body = httplib::body;
namespace net = httplib::net;
namespace http = httplib::http;
namespace mw = httplib::server::middleware;

namespace
{
    using test_common::as_string;
    using test_common::run;
    using test_common::setup_logger;

    template <typename Client>
    net::awaitable<std::string>
    send_chunked(Client& client, http::verb method, std::string_view path, std::vector<std::string> chunks)
    {
        auto writer = client.create_lazy_request();

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

        auto resp = co_await writer->read_response_lazy();
        if (resp.has_error())
        {
            co_return std::string {};
        }

        std::string body;
        std::array<char, 4096> buf;
        for (;;)
        {
            auto result = co_await resp->read_some_raw(net::buffer(buf));
            if (result.has_error() || result.value() == 0)
            {
                break;
            }
            body.append(buf.data(), result.value());
        }
        co_return body;
    }

} // namespace

TEST_CASE("Chunked: Content-Length hits chunked handler", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked-only",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    resp.set_string_content("chunked-handled"sv, "text/plain");
                    co_return;
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/chunked-only", "data"sv));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "chunked-handled");
            co_return;
        });
}

TEST_CASE("Chunked: regular POST takes precedence over chunked", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/chunked/precedence",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& body = req.as_string();
                    resp.set_string_content("regular-" + body, "text/plain");
                });

            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/precedence",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    resp.set_string_content("should-not-run"sv, "text/plain");
                    co_return;
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/chunked/precedence", "data"sv));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "regular-data");
            co_return;
        });
}

TEST_CASE("Chunked: GET coexists with chunked POST", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/chunked/both",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("get-ok"sv, "text/plain"); });

            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/both",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    std::array<char, 4096> buf;
                    auto result = co_await req.read_some_raw(net::buffer(buf));
                    resp.set_string_content("chunked-ok"sv, "text/plain");
                    co_return;
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto get_resp = UNWRAP(co_await client.async_get("/chunked/both"));
            REQUIRE(get_resp.result() == http::status::ok);
            REQUIRE(as_string(get_resp) == "get-ok");

            auto post_resp = UNWRAP(co_await client.async_post("/chunked/both", "data"sv));
            REQUIRE(post_resp.result() == http::status::ok);
            REQUIRE(as_string(post_resp) == "chunked-ok");
            co_return;
        });
}

TEST_CASE("Chunked: is_lazy() false for regular handler", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/chunked/check",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(!req.is_lazy());
                    resp.set_string_content("not-chunked"sv, "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/chunked/check", "data"sv));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "not-chunked");
            co_return;
        });
}

TEST_CASE("Chunked: multi-verb chunked handler registration", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post, http::verb::put>(
                "/chunked/multi",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    std::array<char, 1024> buf;
                    co_await req.read_some_raw(net::buffer(buf));
                    auto method = std::string(req.method_string());
                    resp.set_string_content("chunked-" + method, "text/plain");
                    co_return;
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto post_resp = UNWRAP(co_await client.async_post("/chunked/multi", "data"sv));
            REQUIRE(post_resp.result() == http::status::ok);
            REQUIRE(as_string(post_resp) == "chunked-POST");

            auto put_resp = UNWRAP(co_await client.async_put("/chunked/multi", "data"sv));
            REQUIRE(put_resp.result() == http::status::ok);
            REQUIRE(as_string(put_resp) == "chunked-PUT");
            co_return;
        });
}

TEST_CASE("Chunked: handler with path param", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/user/:id",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto id = req.path_param("id");
                    resp.set_string_content("chunked-" + std::string(id), "text/plain");
                    co_return;
                });
            server.router().template set_http_handler<http::verb::get>(
                "/chunked/user/:id",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto id = req.path_param("id");
                    resp.set_string_content("get-" + std::string(id), "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto get_resp = UNWRAP(co_await client.async_get("/chunked/user/42"));
            REQUIRE(get_resp.result() == http::status::ok);
            REQUIRE(as_string(get_resp) == "get-42");

            auto post_resp = UNWRAP(co_await client.async_post("/chunked/user/42", "data"sv));
            REQUIRE(post_resp.result() == http::status::ok);
            REQUIRE(as_string(post_resp) == "chunked-42");
            co_return;
        });
}

TEST_CASE("Chunked: handler with wildcard path", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/ws/*",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto wild = req.path_param("*");
                    resp.set_string_content("chunked-" + std::string(wild), "text/plain");
                    co_return;
                });
            server.router().template set_http_handler<http::verb::get>(
                "/chunked/ws/*",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto wild = req.path_param("*");
                    resp.set_string_content("get-" + std::string(wild), "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto get_resp = UNWRAP(co_await client.async_get("/chunked/ws/a/b/c"));
            REQUIRE(get_resp.result() == http::status::ok);
            REQUIRE(as_string(get_resp) == "get-a/b/c");

            auto post_resp = UNWRAP(co_await client.async_post("/chunked/ws/x/y", "data"sv));
            REQUIRE(post_resp.result() == http::status::ok);
            REQUIRE(as_string(post_resp) == "chunked-x/y");
            co_return;
        });
}

TEST_CASE("Chunked: middleware is wrapped via set_lazy_http_handler", "[chunked]")
{
    mw::cors_middleware cors_middleware;
    cors_middleware.allow_origins({ "https://example.com" });
    cors_middleware.allow_methods({ "GET", "POST" });

    run(
        [&](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/cors_middleware",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    resp.set_string_content("should-not-run"sv, "text/plain");
                    co_return;
                },
                cors_middleware);
            server.router().template set_http_handler<http::verb::get>(
                "/chunked/cors_middleware",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("cors_middleware-get"sv, "text/plain"); },
                cors_middleware);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::origin, "https://example.com");
            auto get_resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/chunked/cors_middleware", hdrs)));
            REQUIRE(get_resp.result() == http::status::ok);
            REQUIRE(as_string(get_resp) == "cors_middleware-get");
            REQUIRE(get_resp[http::field::access_control_allow_origin] == "https://example.com");
            co_return;
        });
}

TEST_CASE("Chunked: regular PUT coexists with chunked POST", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::put>(
                "/chunked/mixed",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& body = req.as_string();
                    resp.set_string_content("regular-put-" + body, "text/plain");
                });

            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/mixed",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    resp.set_string_content("chunked-post"sv, "text/plain");
                    co_return;
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto put_resp = UNWRAP(co_await client.async_put("/chunked/mixed", "hello"sv));
            REQUIRE(put_resp.result() == http::status::ok);
            REQUIRE(as_string(put_resp) == "regular-put-hello");

            auto post_resp = UNWRAP(co_await client.async_post("/chunked/mixed", "data"sv));
            REQUIRE(post_resp.result() == http::status::ok);
            REQUIRE(as_string(post_resp) == "chunked-post");
            co_return;
        });
}

TEST_CASE("Chunked: chunked handler does not affect path that only has regular handlers", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/isolated",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    resp.set_string_content("should-not-run"sv, "text/plain");
                    co_return;
                });

            server.router().template set_http_handler<http::verb::post>(
                "/regular/path",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& body = req.as_string();
                    resp.set_string_content("regular-" + body, "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/regular/path", "data"sv));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_string(resp) == "regular-data");
            co_return;
        });
}

TEST_CASE("Chunked: buffer_body receives de-chunked data", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/read",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    std::string accumulated;
                    std::array<char, 8192> buf;
                    for (;;)
                    {
                        auto _bytes_r = co_await req.read_some_raw(net::buffer(buf));
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
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto result = co_await send_chunked(client, http::verb::post, "/chunked/read", { "Hello", " World" });
            REQUIRE(result == "Hello World");
            co_return;
        });
}

TEST_CASE("Chunked: multiple chunks are de-chunked into single body", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/read-ext",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    std::string accumulated;
                    std::array<char, 8192> buf;
                    for (;;)
                    {
                        auto _bytes_r = co_await req.read_some_raw(net::buffer(buf));
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
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto result = co_await send_chunked(client,
                                                http::verb::post,
                                                "/chunked/read-ext",
                                                { "chunk1", "chunk2", "chunk3" });
            REQUIRE(result == "chunk1chunk2chunk3");
            co_return;
        });
}

TEST_CASE("Chunked: large chunk via buffer_body", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/read-large",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    std::string accumulated;
                    std::array<char, 8192> buf;
                    for (;;)
                    {
                        auto _bytes_r = co_await req.read_some_raw(net::buffer(buf));
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
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::string large_chunk(10000, 'X');
            auto result = co_await send_chunked(client, http::verb::post, "/chunked/read-large", { large_chunk });
            REQUIRE(result == "10000");
            co_return;
        });
}

TEST_CASE("Chunked: empty chunks via buffer_body", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/read-empty",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    std::string accumulated;
                    std::array<char, 4096> buf;
                    auto bytes_result = co_await req.read_some_raw(net::buffer(buf));
                    resp.set_string_content(std::to_string(accumulated.size()), "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto result = co_await send_chunked(client, http::verb::post, "/chunked/read-empty", {});
            REQUIRE(result == "0");
            co_return;
        });
}

TEST_CASE("Chunked: is_lazy() is true", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/read-is-chunked",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    bool was_body = req.is_lazy();
                    std::array<char, 8192> buf;
                    auto bytes_result = co_await req.read_some_raw(net::buffer(buf));
                    if (bytes_result.has_error())
                    {
                        co_return;
                    }
                    auto bytes = bytes_result.value();
                    resp.set_string_content(std::string(was_body ? "yes:" : "no:") + std::string(buf.data(), bytes),
                                            "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto result = co_await send_chunked(client, http::verb::post, "/chunked/read-is-chunked", { "data" });
            REQUIRE(result == "yes:data");
            co_return;
        });
}

TEST_CASE("Chunked: with path parameters via buffer_body", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/read/:id",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    auto id = std::string(req.path_param("id"));
                    std::string accumulated;
                    std::array<char, 8192> buf;
                    for (;;)
                    {
                        auto _bytes_r = co_await req.read_some_raw(net::buffer(buf));
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
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto result = co_await send_chunked(client, http::verb::post, "/chunked/read/42", { "hello" });
            REQUIRE(result == "42:hello");
            co_return;
        });
}

TEST_CASE("Chunked: with wildcard path via buffer_body", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/read-ws/*",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    auto wild = std::string(req.path_param("*"));
                    std::string accumulated;
                    std::array<char, 8192> buf;
                    for (;;)
                    {
                        auto _bytes_r = co_await req.read_some_raw(net::buffer(buf));
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
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto result = co_await send_chunked(client, http::verb::post, "/chunked/read-ws/a/b", { "xyz" });
            REQUIRE(result == "a/b:xyz");
            co_return;
        });
}

TEST_CASE("Chunked: PUT via buffer_body", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::put>(
                "/chunked/read-put",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    std::string accumulated;
                    std::array<char, 8192> buf;
                    for (;;)
                    {
                        auto _bytes_r = co_await req.read_some_raw(net::buffer(buf));
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
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto result = co_await send_chunked(client, http::verb::put, "/chunked/read-put", { "put-body" });
            REQUIRE(result == "PUT:put-body");
            co_return;
        });
}

TEST_CASE("Chunked: multi-verb via buffer_body", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post, http::verb::patch>(
                "/chunked/read-multi",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    std::string accumulated;
                    std::array<char, 8192> buf;
                    for (;;)
                    {
                        auto _bytes_r = co_await req.read_some_raw(net::buffer(buf));
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
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto post_result = co_await send_chunked(client, http::verb::post, "/chunked/read-multi", { "from-post" });
            REQUIRE(post_result == "POST:from-post");

            auto patch_result
                = co_await send_chunked(client, http::verb::patch, "/chunked/read-multi", { "from-patch" });
            REQUIRE(patch_result == "PATCH:from-patch");
            co_return;
        });
}

TEST_CASE("Chunked: sync send_chunked_request via buffer_body", "[chunked]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/chunked/sync",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    std::string accumulated;
                    std::array<char, 8192> buf;
                    for (;;)
                    {
                        auto _bytes_r = co_await req.read_some_raw(net::buffer(buf));
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
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto result = co_await send_chunked(client, http::verb::post, "/chunked/sync", { "via", "sync" });
            REQUIRE(result == "viasync");
            co_return;
        });
}
