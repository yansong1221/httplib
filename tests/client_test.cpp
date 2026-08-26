#include "common.hpp"
#include "httplib/body/form_data_body.hpp"
#include "httplib/body/json_body.hpp"
#include "httplib/body/query_params_body.hpp"
#include "httplib/body/string_body.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/read_session.hpp"
#include "httplib/client/lazy_request.hpp"
#include "httplib/server/chunk_writer.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/asio/co_spawn.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <thread>

namespace body = httplib::body;
namespace html = httplib::html;
namespace net = httplib::net;

namespace
{

    void
    setup_logger(httplib::server::http_server& server)
    {
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        server.set_logger(std::make_shared<spdlog::logger>("test", null_sink));
    }

    auto
    make_params()
    {
        html::query_params p;
        p.add("msg", "hello");
        return p;
    }

    template <typename Setup, typename Test>
    void
    run(Setup&& setup, Test&& test)
    {
        net::thread_pool pool{ 1 };
        std::exception_ptr err;

        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void>
            {
                httplib::server::http_server server(pool.get_executor());
                setup_logger(server);
                setup(server);
                server.listen("127.0.0.1", 0);
                auto ep = server.local_endpoint();
                server.run();

                httplib::client::http_client client(pool.get_executor(),
                                                     ep.address().to_string(),
                                                     ep.port());
                client.set_timeout(std::chrono::seconds(5));

                co_await test(client);

                client.close();
                server.stop();
            },
            [&](std::exception_ptr e)
            {
                err = e;
            });

        pool.join();
        if (err)
            std::rethrow_exception(err);
    }

    template <typename Setup, typename Test>
    void
    run_pool(Setup&& setup, Test&& test)
    {
        net::thread_pool pool{ 1 };
        std::exception_ptr err;

        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void>
            {
                httplib::server::http_server server(pool.get_executor());
                setup_logger(server);
                setup(server);
                server.listen("127.0.0.1", 0);
                auto ep = server.local_endpoint();
                server.run();

                httplib::client::http_client_pool client_pool(pool.get_executor(), {.max_size = 4});
                client_pool.start();

                co_await test(client_pool, ep);

                client_pool.stop();
                server.stop();
            },
            [&](std::exception_ptr e)
            {
                err = e;
            });

        pool.join();
        if (err)
            std::rethrow_exception(err);
    }

    template <typename Test>
    void
    run_error(Test&& test)
    {
        net::thread_pool pool{ 1 };
        std::exception_ptr err;

        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void> { co_await test(pool); },
            [&](std::exception_ptr e)
            {
                err = e;
            });

        pool.join();
        if (err)
            std::rethrow_exception(err);
    }

#ifdef HTTPLIB_ENABLED_SSL
    template <typename Setup, typename Test>
    void
    run_ssl(Setup&& setup, Test&& test)
    {
        net::thread_pool pool{ 2 };
        std::exception_ptr err;

        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void>
            {
                httplib::server::http_server server(pool.get_executor());
                setup_logger(server);
                setup(server);
                server.set_ssl(kTestCert, kTestKey, "test");
                server.listen("127.0.0.1", 0);
                auto ep = server.local_endpoint();
                server.run();

                httplib::client::http_client client(pool.get_executor(),
                                                      "localhost",
                                                      ep.port(),
                                                      true);
                client.set_timeout(std::chrono::seconds(5));

                co_await test(client);

                client.close();
                server.stop();
            },
            [&](std::exception_ptr e)
            {
                err = e;
            });

        pool.join();
        if (err)
            std::rethrow_exception(err);
    }
#endif

} // namespace

// ===========================================================================
// Client pool
// ===========================================================================

TEST_CASE("client: pool acquire and use", "[client]")
{
    run_pool(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            auto handle = co_await pool.async_acquire(ep.address().to_string(), ep.port(), false);
            REQUIRE(handle);
            auto resp = UNWRAP(co_await handle->async_get("/echo", make_params()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "hello");
        });
}

TEST_CASE("client: pool multiple acquires", "[client]")
{
    run_pool(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            auto h1 = co_await pool.async_acquire(ep.address().to_string(), ep.port(), false);
            REQUIRE(h1);
            auto h2 = co_await pool.async_acquire(ep.address().to_string(), ep.port(), false);
            REQUIRE(h2);
            auto h3 = co_await pool.async_acquire(ep.address().to_string(), ep.port(), false);
            REQUIRE(h3);
            auto r1 = UNWRAP(co_await h1->async_get("/echo", make_params()));
            auto r2 = UNWRAP(co_await h2->async_get("/echo", make_params()));
            auto r3 = UNWRAP(co_await h3->async_get("/echo", make_params()));
            REQUIRE(r1.result() == http::status::ok);
            REQUIRE(r2.result() == http::status::ok);
            REQUIRE(r3.result() == http::status::ok);
        });
}

TEST_CASE("client: pool connection reuse", "[client]")
{
    run_pool(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            httplib::client::http_client* raw = nullptr;
            {
                auto h = co_await pool.async_acquire(ep.address().to_string(), ep.port(), false);
                raw = h.get();
            }
            auto h2 = co_await pool.async_acquire(ep.address().to_string(), ep.port(), false);
            REQUIRE(h2.get() == raw);
            auto resp = UNWRAP(co_await h2->async_get("/echo", make_params()));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("client: pool closed connection reusable", "[client]")
{
    run_pool(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            {
                auto h = co_await pool.async_acquire(ep.address().to_string(), ep.port(), false);
                UNWRAP(co_await h->async_get("/echo", make_params()));
                h->close();
            }
            auto h2 = co_await pool.async_acquire(ep.address().to_string(), ep.port(), false);
            auto resp = UNWRAP(co_await h2->async_get("/echo", make_params()));
            REQUIRE(resp.result() == http::status::ok);
        });
}

// ===========================================================================
// Client basics
// ===========================================================================

TEST_CASE("client: close and is_open", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/echo", make_params()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(client.is_open());
            client.close();
            REQUIRE_FALSE(client.is_open());
        });
}

TEST_CASE("client: host and port", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            REQUIRE(client.host() == "127.0.0.1");
            REQUIRE(client.port() > 0);
            auto resp = UNWRAP(co_await client.async_get("/echo", make_params()));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("client: URL constructor", "[client]")
{
    net::thread_pool pool{ 1 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            httplib::server::http_server server(pool.get_executor());
            setup_logger(server);
            server.router().template set_http_handler<http::verb::get>(
                "/url-test",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("url-ok"sv, "text/plain"); });
            server.listen("127.0.0.1", 0);
            server.run();
            auto ep = server.local_endpoint();
            auto url = std::format("http://{}:{}", ep.address().to_string(), ep.port());
            httplib::client::http_client client(pool.get_executor(), url);
            client.set_timeout(std::chrono::seconds(5));
            auto resp = UNWRAP(co_await client.async_get("/url-test"));
            REQUIRE(resp.result() == http::status::ok);
            server.stop();
        },
        [&](std::exception_ptr e)
        {
            err = e;
        });
    pool.join();
    if (err)
        std::rethrow_exception(err);
}

TEST_CASE("client: logger", "[client]")
{
    run(
        [](auto&) {},
        [](auto& client) -> net::awaitable<void>
        {
            REQUIRE(client.logger() != nullptr);
            auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            auto lg = std::make_shared<spdlog::logger>("custom", sink);
            client.set_logger(lg);
            REQUIRE(client.logger()->name() == "custom");
            co_return;
        });
}

// ===========================================================================
// HTTP method shorthands
// ===========================================================================

TEST_CASE("client: GET with query params", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/echo", make_params()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "hello");
        });
}

TEST_CASE("client: HEAD request", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::head>(
                "/head-test",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set(http::field::content_type, "text/plain");
                    resp.set(http::field::content_length, "4");
                    resp.set_empty_content(http::status::ok);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_head("/head-test"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp[http::field::content_type] == "text/plain");
        });
}

TEST_CASE("client: POST string body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/post-echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(req.body().template as<body::string_body>(), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/post-echo", std::string_view("post-body")));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("client: POST JSON body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/json-echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto val = req.body().template as<body::json_body>();
                    resp.set_json_content(val, http::status::ok);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            boost::json::value body { { "key", "value" }, { "num", 42 } };
            auto resp = UNWRAP(co_await client.async_post("/json-echo", std::move(body)));
            REQUIRE(resp.result() == http::status::ok);
            auto val = resp.body().template as<body::json_body>();
            REQUIRE(val.at("key") == "value");
            REQUIRE(val.at("num") == 42);
        });
}

TEST_CASE("client: PUT string body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::put>(
                "/put-echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(req.body().template as<body::string_body>(), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_put("/put-echo", std::string_view("put-data")));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("client: PATCH string body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::patch>(
                "/patch-echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(req.body().template as<body::string_body>(), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_patch("/patch-echo", std::string_view("patch-data")));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("client: DELETE request", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::delete_>(
                "/delete-test",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("deleted"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_del("/delete-test"));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("client: OPTIONS request", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::options>(
                "/options-test",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set(http::field::allow, "GET, POST, OPTIONS");
                    resp.set_empty_content(http::status::ok);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_options("/options-test"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(!resp[http::field::allow].empty());
        });
}

// ===========================================================================
// Response status codes
// ===========================================================================

TEST_CASE("client: 204 No Content", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/empty-204",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_empty_content(http::status::no_content); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/empty-204"));
            REQUIRE(resp.result() == http::status::no_content);
        });
}

TEST_CASE("client: 304 Not Modified", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/not-modified",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_empty_content(http::status::not_modified); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/not-modified"));
            REQUIRE(resp.result() == http::status::not_modified);
        });
}

TEST_CASE("client: 404 Not Found", "[client]")
{
    run(
        [](auto&) {},
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/non-existent"));
            REQUIRE(resp.result() == http::status::not_found);
        });
}

// ===========================================================================
// Timeout policies
// ===========================================================================

TEST_CASE("client: timeout_policy step", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_timeout_policy(httplib::client::http_client::timeout_policy::step);
            client.set_timeout(std::chrono::seconds(2));
            auto resp = UNWRAP(co_await client.async_get("/echo", make_params()));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("client: timeout_policy never", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_timeout_policy(httplib::client::http_client::timeout_policy::never);
            auto resp = UNWRAP(co_await client.async_get("/echo", make_params()));
            REQUIRE(resp.result() == http::status::ok);
        });
}

// ===========================================================================
// Streaming
// ===========================================================================

TEST_CASE("client: chunked transfer via sessions", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/chunked",
                [](httplib::server::request&,
                   httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.get_chunk_writer();
                    http::fields headers;
                    headers.set(http::field::content_type, "text/plain");
                    co_await cw->write_header(http::status::ok, headers, false);
                    for (int i = 0; i < 5; ++i)
                        co_await cw->write_body(
                            net::buffer(std::string("Chunk") + std::to_string(i)), i < 4);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto writer = client.create_lazy_request();
            co_await writer->write_header(http::verb::get, "/chunked", {});
            co_await writer->write_body(net::buffer("", 0), false);

            auto resp = UNWRAP(co_await writer->read_response());
            std::string streamed;
            std::array<char, 4096> buf;
            while (true)
            {
                auto result = co_await resp.read_some(net::buffer(buf));
                if (result.has_error() || result.value() == 0)
                    break;
                streamed.append(buf.data(), result.value());
            }
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(streamed == "Chunk0Chunk1Chunk2Chunk3Chunk4");
        });
}

TEST_CASE("client: lazy request reads full response", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/echo-full",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto& body = req.body().template as<body::string_body>();
                    resp.set_string_content("echo:" + body, "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto writer = client.create_lazy_request();
            co_await writer->write_header(http::verb::post, "/echo-full", {}, false);
            co_await writer->write_body(net::buffer(std::string_view("hello")), false);

            auto resp = UNWRAP(co_await writer->read_full_response());
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "echo:hello");
        });
}

TEST_CASE("client: has_active_session after request", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/simple",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/simple"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE_FALSE(client.has_active_session());
        });
}

// ===========================================================================
// Download
// ===========================================================================

TEST_CASE("client: download to file", "[client]")
{
    auto srv = std::filesystem::temp_directory_path() / "httplib_dl_server.txt";
    auto dl = std::filesystem::temp_directory_path() / "httplib_dl_output.bin";
    {
        std::ofstream f(srv, std::ios::binary);
        f << "download test content\n";
    }
    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/dl-file",
                [&](httplib::server::request&, httplib::server::response& resp)
                { resp.set_file_content(srv); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_download(http::verb::get, "/dl-file", dl));
            REQUIRE(resp.result() == http::status::ok);
            std::ifstream f(dl, std::ios::binary);
            REQUIRE(f.is_open());
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            REQUIRE(content == "download test content\n");
        });
    std::filesystem::remove(srv);
    std::filesystem::remove(dl);
}

TEST_CASE("client: download randomized round-trip", "[client]")
{
    std::mt19937 rng(789);
    std::uniform_int_distribution<int> size_dist(0, 65536);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (int round = 0; round < 10; ++round)
    {
        int len = size_dist(rng);
        std::string sent;
        sent.reserve(len);
        for (int i = 0; i < len; ++i)
            sent.push_back(static_cast<char>(byte_dist(rng)));
        auto srv = std::filesystem::temp_directory_path()
                   / std::format("httplib_fuzz_srv_{}.bin", round);
        auto dl = std::filesystem::temp_directory_path()
                  / std::format("httplib_fuzz_dl_{}.bin", round);
        {
            std::ofstream f(srv, std::ios::binary);
            f.write(sent.data(), sent.size());
        }
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/dl-fuzz",
                    [&](httplib::server::request&, httplib::server::response& resp)
                    { resp.set_file_content(srv); });
            },
            [&](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(
                    co_await client.async_download(http::verb::get, "/dl-fuzz", dl));
                REQUIRE(resp.result() == http::status::ok);
                std::ifstream f(dl, std::ios::binary);
                std::string received((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                REQUIRE(received == sent);
            });
        std::filesystem::remove(srv);
        std::filesystem::remove(dl);
    }
}

TEST_CASE("client: download with Range", "[client]")
{
    auto srv = std::filesystem::temp_directory_path() / "httplib_dl_range_srv.txt";
    auto dl = std::filesystem::temp_directory_path() / "httplib_dl_range_out.bin";
    {
        std::ofstream f(srv, std::ios::binary);
        f << "0123456789";
    }
    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/dl-range",
                [&](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_file_content(srv, req.base()); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::range, "bytes=0-4");
            auto resp = UNWRAP(
                co_await client.async_download(http::verb::get, "/dl-range", dl, hdrs));
            REQUIRE(resp.result() == http::status::partial_content);
            std::ifstream f(dl, std::ios::binary);
            REQUIRE(f.is_open());
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            REQUIRE(content == "01234");
        });
    std::filesystem::remove(srv);
    std::filesystem::remove(dl);
}

// ===========================================================================
// Redirects
// ===========================================================================

TEST_CASE("client: follows 302 redirect", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/redirect-me",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect("/target", http::status::found); });
            server.router().template set_http_handler<http::verb::get>(
                "/target",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("arrived"sv, "text/plain"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_max_redirects(5);
            auto resp = UNWRAP(co_await client.async_get("/redirect-me"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "arrived");
        });
}

TEST_CASE("client: redirect loop limited", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/loop",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect("/loop", http::status::found); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_max_redirects(3);
            auto resp = UNWRAP(co_await client.async_get("/loop"));
            REQUIRE(resp.result() == http::status::found);
        });
}

TEST_CASE("client: redirect full URL", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ext-redirect",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto port = req.local_endpoint().port();
                    resp.set_redirect(
                        std::format("http://127.0.0.1:{}/target-page", port),
                        http::status::found);
                });
            server.router().template set_http_handler<http::verb::get>(
                "/target-page",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("target-reached"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_max_redirects(1);
            auto resp = UNWRAP(co_await client.async_get("/ext-redirect"));
            REQUIRE(resp.result() == http::status::ok);
        });
}

// ===========================================================================
// Error paths
// ===========================================================================

TEST_CASE("client: connection refused", "[client]")
{
    run_error(
        [](net::thread_pool& pool) -> net::awaitable<void>
        {
            httplib::client::http_client c(pool.get_executor(), "127.0.0.1", 1);
            c.set_timeout(std::chrono::seconds(2));
            auto resp = co_await c.async_get("/");
            REQUIRE_FALSE(resp.has_value());
        });
}

TEST_CASE("client: unreachable host", "[client]")
{
    run_error(
        [](net::thread_pool& pool) -> net::awaitable<void>
        {
            httplib::client::http_client c(pool.get_executor(), "192.0.2.1", 80);
            c.set_timeout(std::chrono::seconds(2));
            auto resp = co_await c.async_get("/");
            REQUIRE_FALSE(resp.has_value());
        });
}

// ===========================================================================
// async_send_request
// ===========================================================================

TEST_CASE("client: async_send_request with headers", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/custom-headers",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto val = req.base()["X-Forwarded-For"];
                    resp.set_string_content(std::string(val), "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            http::fields hdrs;
            hdrs.set("X-Forwarded-For", "10.0.0.1");
            auto resp = UNWRAP(
                co_await client.async_send_request(httplib::client::request(http::verb::get, "/custom-headers", hdrs)));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("client: async_send_request form_data", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/form-upload",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto& fd = req.body().template as<body::form_data_body>();
                    auto fld = fd.field_by_name("name");
                    resp.set_string_content(fld.has_value() ? fld->content : "missing",
                                            "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            html::form_data form;
            form.boundary = "----TestFormBoundary";
            form.fields.push_back({ "name", "", "text/plain", "alice" });
            auto req = httplib::client::request(http::verb::post, "/form-upload");
            req.set_body(std::move(form));
            auto resp = UNWRAP(co_await client.async_send_request(std::move(req)));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("client: async_send_request query_params body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/form-post",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto& qp = req.body().template as<body::query_params_body>();
                    resp.set_string_content(std::string(qp.at("key")), "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            html::query_params body;
            body.add("key", "url-value");
            auto req = httplib::client::request(http::verb::post, "/form-post");
            req.set_body(std::move(body));
            auto resp = UNWRAP(co_await client.async_send_request(std::move(req)));
            REQUIRE(resp.result() == http::status::ok);
        });
}

// ===========================================================================
// send file body
// ===========================================================================

TEST_CASE("client: send file body upload", "[client]")
{
    auto up = std::filesystem::temp_directory_path() / "httplib_upload.bin";
    {
        std::ofstream f(up, std::ios::binary);
        f << "file-content-here";
    }
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::put>(
                "/upload",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(req.body().template as<body::string_body>(), "text/plain"); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto req = httplib::client::request(http::verb::put, "/upload");
            req.set_file_body(up);
            auto resp = UNWRAP(co_await client.async_send_request(std::move(req)));
            REQUIRE(resp.result() == http::status::ok);
        });
    std::filesystem::remove(up);
}

// ===========================================================================
// async_send_request_lazy
// ===========================================================================

TEST_CASE("client: lazy read text", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/lazy-text",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("lazy-hello"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/lazy-text")));
            REQUIRE(resp.result() == http::status::ok);
            auto text = co_await resp.read_text();
            REQUIRE(text == "lazy-hello");
        });
}

TEST_CASE("client: lazy read json", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/lazy-json",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    boost::json::value v { { "key", "value" }, { "num", 7 } };
                    resp.set_json_content(std::move(v));
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/lazy-json")));
            REQUIRE(resp.result() == http::status::ok);
            auto val = co_await resp.read_json();
            REQUIRE(val.at("key") == "value");
            REQUIRE(val.at("num") == 7);
        });
}

TEST_CASE("client: lazy read body typed", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/lazy-body",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("lazy-body"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/lazy-body")));
            auto body = co_await resp.read_body();
            REQUIRE(body.template as<body::string_body>() == "lazy-body");
        });
}

TEST_CASE("client: lazy read multipart body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/lazy-form",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    std::vector<html::form_data::field> fields;
                    auto& a = fields.emplace_back();
                    a.name = "a";
                    a.content = std::string(9000, 'x') + "\r\ncc\r";
                    auto& b = fields.emplace_back();
                    b.name = "b";
                    b.content = "2";
                    resp.set_form_data_content(std::move(fields));
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/lazy-form")));
            auto body = co_await resp.read_body();
            auto& fd = body.template as<body::form_data_body>();
            REQUIRE(fd.fields.size() == 2);
            REQUIRE(fd.fields[0].name == "a");
            REQUIRE(fd.fields[0].content == std::string(9000, 'x') + "\r\ncc\r");
            REQUIRE(fd.fields[1].name == "b");
            REQUIRE(fd.fields[1].content == "2");
        });
}

TEST_CASE("client: lazy read to file", "[client]")
{
    auto save = std::filesystem::temp_directory_path() / "httplib_lazy_download.bin";
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/lazy-file",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("lazy-file-content"sv, "text/plain"); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/lazy-file")));
            auto ec = co_await resp.read_to_file(save);
            REQUIRE(!ec);
        });
    std::string content;
    {
        std::ifstream f(save, std::ios::binary);
        content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    REQUIRE(content == "lazy-file-content");
    std::filesystem::remove(save);
}

// ===========================================================================
// SSL
// ===========================================================================

#ifdef HTTPLIB_ENABLED_SSL

TEST_CASE("client: SSL verify off with set_verify_ssl", "[client]")
{
    run_ssl(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ssl-test",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ssl-ok"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_verify_ssl(false);
            auto resp = co_await client.async_get("/ssl-test");
            REQUIRE(resp.has_value());
            REQUIRE(resp->result() == http::status::ok);
        });
}

TEST_CASE("client: set_verify_ssl fails self-signed", "[client]")
{
    run_ssl(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ssl-verify",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ssl-ok"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_verify_ssl(true);
            auto resp = co_await client.async_get("/ssl-verify");
            REQUIRE(!resp.has_value());
        });
}

TEST_CASE("client: verify with custom CA cert", "[client]")
{
    run_ssl(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ssl-ca",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ssl-ca-ok"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_verify_ssl(true);
            client.set_ca_cert(kTestCert);
            auto resp = co_await client.async_get("/ssl-ca");
            REQUIRE(resp.has_value());
            REQUIRE(resp->result() == http::status::ok);
        });
}

TEST_CASE("client: SSL verify enabled by default", "[client]")
{
    run_ssl(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ssl-default",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ssl-ok"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_ca_cert(kTestCert);
            auto resp = co_await client.async_get("/ssl-default");
            REQUIRE(resp.has_value());
            REQUIRE(resp->result() == http::status::ok);
        });
}

#endif
