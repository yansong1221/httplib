#include "common.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/lazy_request.hpp"
#include "httplib/client/stream_reader.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/stream_writer.hpp"
#include <boost/asio/co_spawn.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <thread>

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
        httplib::query_params p;
        p.add("msg", "hello");
        return p;
    }

    template <typename Setup, typename Test>
    void
    run(Setup&& setup, Test&& test)
    {
        net::thread_pool pool { 1 };
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

                httplib::client::http_client client(pool.get_executor(), ep.address().to_string(), ep.port());
                client.set_timeout(std::chrono::seconds(5));

                co_await test(client);

                client.close();
                server.stop();
            },
            [&](std::exception_ptr e) { err = e; });

        pool.join();
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

    template <typename Setup, typename Test>
    void
    run_pool(Setup&& setup, Test&& test)
    {
        net::thread_pool pool { 1 };
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

                httplib::client::http_client_pool client_pool(pool.get_executor(), { .max_size = 4 });

                co_await test(client_pool, ep);

                client_pool.stop();
                server.stop();
            },
            [&](std::exception_ptr e) { err = e; });

        pool.join();
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

    template <typename Test>
    void
    run_error(Test&& test)
    {
        net::thread_pool pool { 1 };
        std::exception_ptr err;

        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void> { co_await test(pool); },
            [&](std::exception_ptr e) { err = e; });

        pool.join();
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

#ifdef HTTPLIB_ENABLED_SSL
    template <typename Setup, typename Test>
    void
    run_ssl(Setup&& setup, Test&& test)
    {
        net::thread_pool pool { 2 };
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
                                                    httplib::url::scheme::tls);
                client.set_timeout(std::chrono::seconds(5));

                co_await test(client);

                client.close();
                server.stop();
            },
            [&](std::exception_ptr e) { err = e; });

        pool.join();
        if (err)
        {
            std::rethrow_exception(err);
        }
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
            server.router().template set_http_handler<httplib::method::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            auto handle = co_await pool.async_acquire(ep.address().to_string(), ep.port(), httplib::url::scheme::plain);
            REQUIRE(handle);
            auto resp = UNWRAP(co_await handle->async_get("/echo", make_params()));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "hello");
        });
}

TEST_CASE("client: pool multiple acquires", "[client]")
{
    run_pool(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            auto h1 = co_await pool.async_acquire(ep.address().to_string(), ep.port(), httplib::url::scheme::plain);
            REQUIRE(h1);
            auto h2 = co_await pool.async_acquire(ep.address().to_string(), ep.port(), httplib::url::scheme::plain);
            REQUIRE(h2);
            auto h3 = co_await pool.async_acquire(ep.address().to_string(), ep.port(), httplib::url::scheme::plain);
            REQUIRE(h3);
            auto r1 = UNWRAP(co_await h1->async_get("/echo", make_params()));
            auto r2 = UNWRAP(co_await h2->async_get("/echo", make_params()));
            auto r3 = UNWRAP(co_await h3->async_get("/echo", make_params()));
            REQUIRE(r1.result() == httplib::status::ok);
            REQUIRE(r2.result() == httplib::status::ok);
            REQUIRE(r3.result() == httplib::status::ok);
        });
}

TEST_CASE("client: pool connection reuse", "[client]")
{
    run_pool(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            httplib::client::http_client* raw = nullptr;
            {
                auto h = co_await pool.async_acquire(ep.address().to_string(), ep.port(), httplib::url::scheme::plain);
                raw = h.get();
            }
            auto h2 = co_await pool.async_acquire(ep.address().to_string(), ep.port(), httplib::url::scheme::plain);
            REQUIRE(h2.get() == raw);
            auto resp = UNWRAP(co_await h2->async_get("/echo", make_params()));
            REQUIRE(resp.result() == httplib::status::ok);
        });
}

TEST_CASE("client: pool closed connection reusable", "[client]")
{
    run_pool(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            {
                auto h = co_await pool.async_acquire(ep.address().to_string(), ep.port(), httplib::url::scheme::plain);
                UNWRAP(co_await h->async_get("/echo", make_params()));
                h->close();
            }
            auto h2 = co_await pool.async_acquire(ep.address().to_string(), ep.port(), httplib::url::scheme::plain);
            auto resp = UNWRAP(co_await h2->async_get("/echo", make_params()));
            REQUIRE(resp.result() == httplib::status::ok);
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
            server.router().template set_http_handler<httplib::method::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/echo", make_params()));
            REQUIRE(resp.result() == httplib::status::ok);
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
            server.router().template set_http_handler<httplib::method::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            REQUIRE(client.host() == "127.0.0.1");
            REQUIRE(client.port() > 0);
            auto resp = UNWRAP(co_await client.async_get("/echo", make_params()));
            REQUIRE(resp.result() == httplib::status::ok);
        });
}

TEST_CASE("client: URL constructor", "[client]")
{
    net::thread_pool pool { 1 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            httplib::server::http_server server(pool.get_executor());
            setup_logger(server);
            server.router().template set_http_handler<httplib::method::get>(
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
            REQUIRE(resp.result() == httplib::status::ok);
            server.stop();
        },
        [&](std::exception_ptr e) { err = e; });
    pool.join();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("client: logger", "[client]")
{
    run([](auto&) {},
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
            server.router().template set_http_handler<httplib::method::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/echo", make_params()));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "hello");
        });
}

TEST_CASE("client: HEAD request", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::head>(
                "/head-test",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set(httplib::field::content_type, "text/plain");
                    resp.set(httplib::field::content_length, "4");
                    resp.set_empty_content(httplib::status::ok);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_head("/head-test"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp[httplib::field::content_type] == "text/plain");
        });
}

TEST_CASE("client: POST string body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::post>(
                "/post-echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(req.as_string(), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/post-echo", std::string_view("post-body"), "text/plain"sv));
            REQUIRE(resp.result() == httplib::status::ok);
        });
}

TEST_CASE("client: POST JSON body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::post>(
                "/json-echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto val = req.as_json();
                    resp.set_json_content(val, httplib::status::ok);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            boost::json::value body {
                { "key", "value" },
                { "num",      42 }
            };
            auto resp = UNWRAP(co_await client.async_post("/json-echo", std::move(body)));
            REQUIRE(resp.result() == httplib::status::ok);
            auto val = resp.as_json();
            REQUIRE(val.at("key") == "value");
            REQUIRE(val.at("num") == 42);
        });
}

TEST_CASE("client: PUT string body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::put>(
                "/put-echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(req.as_string(), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_put("/put-echo", std::string_view("put-data"), "text/plain"sv));
            REQUIRE(resp.result() == httplib::status::ok);
        });
}

TEST_CASE("client: PATCH string body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::patch>(
                "/patch-echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(req.as_string(), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp
                = UNWRAP(co_await client.async_patch("/patch-echo", std::string_view("patch-data"), "text/plain"sv));
            REQUIRE(resp.result() == httplib::status::ok);
        });
}

TEST_CASE("client: DELETE request", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::delete_>(
                "/delete-test",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("deleted"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_del("/delete-test"));
            REQUIRE(resp.result() == httplib::status::ok);
        });
}

TEST_CASE("client: OPTIONS request", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::options>(
                "/options-test",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set(httplib::field::allow, "GET, POST, OPTIONS");
                    resp.set_empty_content(httplib::status::ok);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_options("/options-test"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(!resp[httplib::field::allow].empty());
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
            server.router().template set_http_handler<httplib::method::get>(
                "/empty-204",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_empty_content(httplib::status::no_content); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/empty-204"));
            REQUIRE(resp.result() == httplib::status::no_content);
        });
}

TEST_CASE("client: 304 Not Modified", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/not-modified",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_empty_content(httplib::status::not_modified); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/not-modified"));
            REQUIRE(resp.result() == httplib::status::not_modified);
        });
}

TEST_CASE("client: 404 Not Found", "[client]")
{
    run([](auto&) {},
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/non-existent"));
            REQUIRE(resp.result() == httplib::status::not_found);
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
            server.router().template set_http_handler<httplib::method::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_timeout_policy(httplib::client::http_client::timeout_policy::step);
            client.set_timeout(std::chrono::seconds(2));
            auto resp = UNWRAP(co_await client.async_get("/echo", make_params()));
            REQUIRE(resp.result() == httplib::status::ok);
        });
}

TEST_CASE("client: timeout_policy never", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(std::string(req.query_params().at("msg")), "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_timeout_policy(httplib::client::http_client::timeout_policy::never);
            auto resp = UNWRAP(co_await client.async_get("/echo", make_params()));
            REQUIRE(resp.result() == httplib::status::ok);
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
            server.router().template set_http_handler<httplib::method::get>(
                "/chunked",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.create_stream_writer();
                    httplib::headers headers;
                    headers.set(httplib::field::content_type, "text/plain");
                    co_await cw->write_header(httplib::status::ok, headers, httplib::server::stream_writer::mode::chunked);
                    for (int i = 0; i < 5; ++i)
                    {
                        co_await cw->write_body(net::buffer(std::string("Chunk") + std::to_string(i)), i < 4);
                    }
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto writer = client.create_lazy_request();
            co_await writer->write_header(httplib::method::get, "/chunked", {}, httplib::client::lazy_request::mode::relay);
            co_await writer->write_body(net::buffer("", 0), false);

            auto resp = UNWRAP(co_await writer->read_response_lazy());
            std::string streamed;
            std::array<char, 4096> buf;
            while (true)
            {
                boost::system::error_code ec;
                auto result = co_await resp.read_some_raw(net::buffer(buf), ec);
                if (ec || result == 0)
                {
                    break;
                }
                streamed.append(buf.data(), result);
            }
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(streamed == "Chunk0Chunk1Chunk2Chunk3Chunk4");
        });
}

TEST_CASE("client: lazy request reads full response", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::post>(
                "/echo-full",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& body = req.as_string();
                    resp.set_string_content("echo:" + body, "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto writer = client.create_lazy_request();
            co_await writer->write_header(httplib::method::post,
                                          "/echo-full",
                                          {},
                                          httplib::client::lazy_request::mode::chunked);
            co_await writer->write_body(net::buffer(std::string_view("hello")), false);

            auto resp = UNWRAP(co_await writer->read_response());
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "echo:hello");
        });
}

TEST_CASE("client: has_active_session after request", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/simple",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/simple"));
            REQUIRE(resp.result() == httplib::status::ok);
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
            server.router().template set_http_handler<httplib::method::get>(
                "/dl-file",
                [&](httplib::server::request&, httplib::server::response& resp) { resp.set_file_content(srv); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_download(httplib::method::get, "/dl-file", dl));
            REQUIRE(resp.result() == httplib::status::ok);
            std::ifstream f(dl, std::ios::binary);
            REQUIRE(f.is_open());
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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
        {
            sent.push_back(static_cast<char>(byte_dist(rng)));
        }
        auto srv = std::filesystem::temp_directory_path() / std::format("httplib_fuzz_srv_{}.bin", round);
        auto dl = std::filesystem::temp_directory_path() / std::format("httplib_fuzz_dl_{}.bin", round);
        {
            std::ofstream f(srv, std::ios::binary);
            f.write(sent.data(), sent.size());
        }
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<httplib::method::get>(
                    "/dl-fuzz",
                    [&](httplib::server::request&, httplib::server::response& resp) { resp.set_file_content(srv); });
            },
            [&](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_download(httplib::method::get, "/dl-fuzz", dl));
                REQUIRE(resp.result() == httplib::status::ok);
                std::ifstream f(dl, std::ios::binary);
                std::string received((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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
            server.router().template set_http_handler<httplib::method::get>(
                "/dl-range",
                [&](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_file_content(srv, req.base()); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::range, "bytes=0-4");
            auto resp = UNWRAP(co_await client.async_download(httplib::method::get, "/dl-range", dl, hdrs));
            REQUIRE(resp.result() == httplib::status::partial_content);
            std::ifstream f(dl, std::ios::binary);
            REQUIRE(f.is_open());
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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
            server.router().template set_http_handler<httplib::method::get>(
                "/redirect-me",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect("/target", httplib::status::found); });
            server.router().template set_http_handler<httplib::method::get>(
                "/target",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("arrived"sv, "text/plain"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_max_redirects(5);
            auto resp = UNWRAP(co_await client.async_get("/redirect-me"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "arrived");
        });
}

TEST_CASE("client: redirect loop limited", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/loop",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect("/loop", httplib::status::found); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_max_redirects(3);
            auto resp = UNWRAP(co_await client.async_get("/loop"));
            REQUIRE(resp.result() == httplib::status::found);
        });
}

TEST_CASE("client: redirect full URL", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ext-redirect",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto port = req.local_endpoint().port();
                    resp.set_redirect(std::format("http://127.0.0.1:{}/target-page", port), httplib::status::found);
                });
            server.router().template set_http_handler<httplib::method::get>(
                "/target-page",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("target-reached"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_max_redirects(1);
            auto resp = UNWRAP(co_await client.async_get("/ext-redirect"));
            REQUIRE(resp.result() == httplib::status::ok);
        });
}

TEST_CASE("client: strips sensitive headers on cross-origin redirect", "[client]")
{
    net::thread_pool pool { 2 };
    std::exception_ptr err;
    std::atomic<bool> leaked_auth = false;
    std::atomic<bool> leaked_cookie = false;

    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            httplib::server::http_server target(pool.get_executor());
            setup_logger(target);
            target.router().template set_http_handler<httplib::method::get>(
                "/target",
                [&](httplib::server::request& req, httplib::server::response& resp)
                {
                    leaked_auth = req.has(httplib::field::authorization);
                    leaked_cookie = req.has(httplib::field::cookie);
                    resp.set_string_content("target-ok"sv, "text/plain");
                });
            target.listen("127.0.0.1", 0);
            auto target_port = target.local_endpoint().port();
            target.run();

            httplib::server::http_server origin(pool.get_executor());
            setup_logger(origin);
            origin.router().template set_http_handler<httplib::method::get>(
                "/start",
                [&](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect(std::format("http://127.0.0.1:{}/target", target_port), httplib::status::found); });
            origin.listen("127.0.0.1", 0);
            auto origin_port = origin.local_endpoint().port();
            origin.run();

            httplib::client::http_client client(pool.get_executor(), "127.0.0.1", origin_port);
            client.set_timeout(std::chrono::seconds(5));
            client.set_max_redirects(2);

            auto req = httplib::client::request(httplib::method::get, "/start");
            req.set(httplib::field::authorization, "Bearer secret");
            req.set(httplib::field::cookie, "session=abc");
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "target-ok");
            REQUIRE_FALSE(leaked_auth.load());
            REQUIRE_FALSE(leaked_cookie.load());

            client.close();
            origin.stop();
            target.stop();
        },
        [&](std::exception_ptr e) { err = e; });

    pool.join();
    if (err)
    {
        std::rethrow_exception(err);
    }
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
            server.router().template set_http_handler<httplib::method::get>(
                "/custom-headers",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto val = req.base()["X-Forwarded-For"];
                    resp.set_string_content(std::string(val), "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::headers hdrs;
            hdrs.set("X-Forwarded-For", "10.0.0.1");
            httplib::client::request req(httplib::method::get, "/custom-headers", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::ok);
        });
}

TEST_CASE("client: async_send_request form_data", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::post>(
                "/form-upload",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& fd = req.as_form_data();
                    auto fld = fd.field_by_name("name");
                    resp.set_string_content(fld.has_value() ? fld->content : "missing", "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::form_data form;
            form.boundary = "----TestFormBoundary";
            form.fields.push_back({ "name", "", "text/plain", "alice" });
            auto req = httplib::client::request(httplib::method::post, "/form-upload");
            req.set_body(std::move(form));
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::ok);
        });
}

TEST_CASE("client: async_send_request query_params body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::post>(
                "/form-post",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& qp = req.as_query_params();
                    resp.set_string_content(std::string(qp.at("key")), "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::query_params body;
            body.add("key", "url-value");
            auto req = httplib::client::request(httplib::method::post, "/form-post");
            req.set_body(std::move(body));
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::ok);
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
            server.router().template set_http_handler<httplib::method::put>(
                "/upload",
                [](httplib::server::request& req, httplib::server::response& resp)
                { resp.set_string_content(req.as_string(), "text/plain"); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto req = httplib::client::request(httplib::method::put, "/upload");
            boost::system::error_code ec;
            req.set_file_body(up, ec);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::ok);
        });
    std::filesystem::remove(up);
}

// ===========================================================================
// async_send_request (lazy mode)
// ===========================================================================

TEST_CASE("client: lazy read text", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/lazy-text",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("lazy-hello"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/lazy-text");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto text = UNWRAP(co_await resp.read_string());
            REQUIRE(text == "lazy-hello");
        });
}

// 回归：空 body + Content-Encoding 必须读成空串，而不是报错。
// server 协商压缩时只检查 Accept-Encoding 与 content-type，不检查 body 是否为空
// （session.cpp），所以空 body 也会带上 Content-Encoding: gzip 与 Content-Length: 0。
// 客户端曾对这种"从未收到过压缩字节"的解码器调 finish()，得到
// "compression stream truncated or insufficient input"，read_string() 返回错误。
// 读取失败还会波及连接复用，所以这里额外验证同一 client 的下一个请求仍然成功。
TEST_CASE("client: empty body with Content-Encoding reads as empty string", "[client]")
{
    SKIP_WITHOUT_COMPRESS();
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/empty-encoded",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content(std::string {}, "text/plain"); });
            server.router().template set_http_handler<httplib::method::get>(
                "/after-empty-encoded",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto headers = httplib::headers();
            headers.set(httplib::field::accept_encoding, "gzip");

            httplib::client::request req(httplib::method::get, "/empty-encoded", headers);
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            // 确认这条路径真的产出了"空 body 却带 Content-Encoding"，否则本测试是空转。
            REQUIRE(resp[httplib::field::content_encoding] == "gzip");
            REQUIRE(resp[httplib::field::content_length] == "0");

            auto text = co_await resp.read_string();
            REQUIRE(text);
            REQUIRE(text->empty());

            // 读失败曾污染连接池，使同一 client 的后续请求全部失败。
            auto next = UNWRAP(co_await client.async_get("/after-empty-encoded"));
            REQUIRE(next.result() == httplib::status::ok);
            auto next_text = UNWRAP(co_await next.read_string());
            REQUIRE(next_text == "ok");
        });
}

TEST_CASE("client: lazy read json", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/lazy-json",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    boost::json::value v {
                        { "key", "value" },
                        { "num",       7 }
                    };
                    resp.set_json_content(std::move(v));
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/lazy-json");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto val = UNWRAP(co_await resp.read_json());
            REQUIRE(val.at("key") == "value");
            REQUIRE(val.at("num") == 7);
        });
}

TEST_CASE("client: lazy read body typed", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/lazy-body",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("lazy-body"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/lazy-body");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            auto text = UNWRAP(co_await resp.read_string());
            REQUIRE(text == "lazy-body");
        });
}

TEST_CASE("client: lazy read multipart body", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/lazy-form",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    std::vector<httplib::form_data::field> fields;
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
            httplib::client::request req(httplib::method::get, "/lazy-form");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            auto fd = UNWRAP(co_await resp.read_form_data());
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
            server.router().template set_http_handler<httplib::method::get>(
                "/lazy-file",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("lazy-file-content"sv, "text/plain"); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/lazy-file");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
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

TEST_CASE("client: lazy redirect", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/lazy-redirect-me",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect("/lazy-target", httplib::status::found); });
            server.router().template set_http_handler<httplib::method::get>(
                "/lazy-target",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("lazy-arrived"sv, "text/plain"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_max_redirects(5);
            httplib::client::request req(httplib::method::get, "/lazy-redirect-me");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto text = UNWRAP(co_await resp.read_string());
            REQUIRE(text == "lazy-arrived");
        });
}

TEST_CASE("client: lazy redirect loop limited", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/lazy-loop",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect("/lazy-loop", httplib::status::found); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_max_redirects(3);
            httplib::client::request req(httplib::method::get, "/lazy-loop");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::found);
        });
}

TEST_CASE("client: lazy redirect full URL", "[client]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/lazy-ext-redirect",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto port = req.local_endpoint().port();
                    resp.set_redirect(std::format("http://127.0.0.1:{}/lazy-target-page", port), httplib::status::found);
                });
            server.router().template set_http_handler<httplib::method::get>(
                "/lazy-target-page",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("lazy-target-reached"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_max_redirects(1);
            httplib::client::request req(httplib::method::get, "/lazy-ext-redirect");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto text = UNWRAP(co_await resp.read_string());
            REQUIRE(text == "lazy-target-reached");
        });
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
            server.router().template set_http_handler<httplib::method::get>(
                "/ssl-test",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ssl-ok"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_verify_ssl(false);
            auto resp = co_await client.async_get("/ssl-test");
            REQUIRE(resp.has_value());
            REQUIRE(resp->result() == httplib::status::ok);
        });
}

TEST_CASE("client: set_verify_ssl fails self-signed", "[client]")
{
    run_ssl(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
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
            server.router().template set_http_handler<httplib::method::get>(
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
            REQUIRE(resp->result() == httplib::status::ok);
        });
}

TEST_CASE("client: SSL verify enabled by default", "[client]")
{
    run_ssl(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ssl-default",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ssl-ok"sv, "text/plain"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            client.set_ca_cert(kTestCert);
            auto resp = co_await client.async_get("/ssl-default");
            REQUIRE(resp.has_value());
            REQUIRE(resp->result() == httplib::status::ok);
        });
}

#endif
