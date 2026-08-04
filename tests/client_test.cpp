#include "common.hpp"
#include "httplib/body/string_body.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/read_session.hpp"
#include "httplib/client/write_session.hpp"
#include "httplib/server/chunk_writer.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>

using namespace std::string_view_literals;
namespace http = httplib::http;
namespace net = httplib::net;
namespace beast = httplib::beast;

namespace
{

    struct test_scaffold
    {
        net::io_context ioc;
        httplib::server::http_server server;
        httplib::tcp::endpoint endpoint;
        std::thread thread;

        test_scaffold() : server(ioc)
        {
            auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            server.set_logger(std::make_shared<spdlog::logger>("httplib.tests", null_sink));
        }

        ~test_scaffold()
        {
            server.stop().wait();
            ioc.stop();
            if (thread.joinable())
            {
                thread.join();
            }
        }

        void
        start_with_routes()
        {
            server.router().set_http_handler<http::verb::get>(
                "/echo",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto msg = std::string(req.query_params().at("msg"));
                    resp.set_string_content(msg, "text/plain");
                });
            server.router().set_http_handler<http::verb::get>(
                "/slow",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("done"sv, "text/plain"sv); });

            server.listen("127.0.0.1", 0);
            endpoint = server.local_endpoint();
            server.run();
            thread = std::thread([this] { ioc.run(); });
        }

        std::unique_ptr<httplib::client::http_client>
        make_client()
        {
            auto c = std::make_unique<httplib::client::http_client>(ioc.get_executor(),
                                                                    endpoint.address().to_string(),
                                                                    endpoint.port());
            c->set_timeout(std::chrono::seconds(5));
            return c;
        }

        auto&
        router()
        {
            return server.router();
        }

        std::string
        host() const
        {
            return endpoint.address().to_string();
        }
        uint16_t
        port() const
        {
            return endpoint.port();
        }

        net::any_io_executor
        executor()
        {
            return ioc.get_executor();
        }
    };

    using test_common::as_string;

} // namespace

TEST_CASE("Client pool: acquire and use a connection", "[client]")
{
    auto params = httplib::html::query_params();
    params.add("msg", "hello");

    test_scaffold ts;
    ts.start_with_routes();

    {
        httplib::client::http_client_pool pool(ts.executor(), 4);
        auto handle = pool.acquire(ts.host(), ts.port()).get();
        REQUIRE(handle);
        auto resp = UNWRAP(handle->get("/echo", params));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp) == "hello");
    }

    // Verify client is still usable after pool is gone
    auto client = ts.make_client();
    auto resp = UNWRAP(client->get("/echo", params));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "hello");
}

TEST_CASE("Client pool: multiple acquires", "[client]")
{
    auto params = httplib::html::query_params();
    params.add("msg", "world");

    test_scaffold ts;
    ts.start_with_routes();

    httplib::client::http_client_pool pool(ts.executor(), 4);
    auto h1 = pool.acquire(ts.host(), ts.port()).get();
    REQUIRE(h1);
    auto h2 = pool.acquire(ts.host(), ts.port()).get();
    REQUIRE(h2);
    auto h3 = pool.acquire(ts.host(), ts.port()).get();
    REQUIRE(h3);

    auto r1 = UNWRAP(h1->get("/echo", params));
    auto r2 = UNWRAP(h2->get("/echo", params));
    auto r3 = UNWRAP(h3->get("/echo", params));
    REQUIRE(r1.result() == http::status::ok);
    REQUIRE(r2.result() == http::status::ok);
    REQUIRE(r3.result() == http::status::ok);
}

TEST_CASE("Client pool: connection reuse", "[client]")
{
    auto params = httplib::html::query_params();
    params.add("msg", "test");

    test_scaffold ts;
    ts.start_with_routes();

    httplib::client::http_client_pool pool(ts.executor(), 4);
    auto* raw = [&]
    {
        auto h = pool.acquire(ts.host(), ts.port()).get();
        return h.get();
    }(); // handle destroyed, connection returned to pool

    // Re-acquire should get the same connection back
    auto h2 = pool.acquire(ts.host(), ts.port()).get();
    REQUIRE(h2.get() == raw);
    auto resp = UNWRAP(h2->get("/echo", params));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Client pool: stats reflects idle count", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    httplib::client::http_client_pool pool(ts.executor(), 4);
    {
        auto h1 = pool.acquire(ts.host(), ts.port()).get();
        auto h2 = pool.acquire(ts.host(), ts.port()).get();
        // h1, h2 in use, none idle
        auto s = pool.stats(ts.host(), ts.port());
        REQUIRE(s.idle == 0);
    }
    // Both released
    auto s = pool.stats(ts.host(), ts.port());
    REQUIRE(s.idle == 2);
}

TEST_CASE("Client pool: respects max_size", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    httplib::client::http_client_pool pool(ts.executor(), 3);
    {
        auto h1 = pool.acquire(ts.host(), ts.port()).get();
        auto h2 = pool.acquire(ts.host(), ts.port()).get();
        auto h3 = pool.acquire(ts.host(), ts.port()).get();
    }
    auto s = pool.stats(ts.host(), ts.port());
    REQUIRE(s.idle == 3);
}

TEST_CASE("Client pool: closed connection still reusable", "[client]")
{
    auto params = httplib::html::query_params();
    params.add("msg", "x");

    test_scaffold ts;
    ts.start_with_routes();

    httplib::client::http_client_pool pool(ts.executor(), 4);
    auto* raw = [&]
    {
        auto h = pool.acquire(ts.host(), ts.port()).get();
        auto p = h.get();
        UNWRAP(h->get("/echo", params));
        return p;
    }();
    raw->close();

    // Re-acquire — gets the same connection, reconnects on next request
    auto h2 = pool.acquire(ts.host(), ts.port()).get();
    auto resp = UNWRAP(h2->get("/echo", params));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Client: close and is_open", "[client]")
{
    auto params = httplib::html::query_params();
    params.add("msg", "hello");

    test_scaffold ts;
    ts.start_with_routes();

    auto client = ts.make_client();
    auto resp = UNWRAP(client->get("/echo", params));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "hello");
    REQUIRE(client->is_open());
    client->close();
    REQUIRE_FALSE(client->is_open());
}

TEST_CASE("Client: custom chunk handler", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/chunked",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto cw = resp.get_chunk_writer();
            http::fields headers;
            headers.set(http::field::content_type, "text/plain");
            co_await cw->write_header(http::status::ok, headers, false);
            for (int i = 0; i < 5; ++i)
            {
                co_await cw->write_body(net::buffer(std::string("Chunk") + std::to_string(i)), i < 4);
            }
        });
    ts.start_with_routes();

    std::string streamed;
    auto client = ts.make_client();
    auto writer = client->create_writer();
    auto reader = client->create_reader();

    boost::asio::co_spawn(
        ts.ioc,
        [&]() -> net::awaitable<void>
        {
            co_await writer->write_header(http::verb::get, "/chunked", {});
            co_await writer->write_body(net::buffer("", 0), false);
            co_await reader->read_header();

            std::array<char, 4096> buf;
            while (true)
            {
                auto result = co_await reader->read_body(net::buffer(buf));
                if (result.has_error() || result.value() == 0)
                {
                    break;
                }
                streamed.append(buf.data(), result.value());
            }
        },
        boost::asio::use_future)
        .get();

    REQUIRE(reader->result() == http::status::ok);
    REQUIRE(streamed == "Chunk0Chunk1Chunk2Chunk3Chunk4");
}

TEST_CASE("Client host and port", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    auto client = ts.make_client();
    REQUIRE(client->host() == ts.host());
    REQUIRE(client->port() == ts.port());
}

TEST_CASE("Client: timeout_policy step", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    auto client = ts.make_client();
    client->set_timeout_policy(httplib::client::http_client::timeout_policy::step);
    client->set_timeout(std::chrono::seconds(2));
    auto params = httplib::html::query_params();
    params.add("msg", "hello");
    auto resp = UNWRAP(client->get("/echo", params));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Client: timeout_policy never", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    auto client = ts.make_client();
    client->set_timeout_policy(httplib::client::http_client::timeout_policy::never);
    auto params = httplib::html::query_params();
    params.add("msg", "hello");
    auto resp = UNWRAP(client->get("/echo", params));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Client: HEAD request", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::head>("/head-test",
                                                   [](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_type, "text/plain");
                                                       resp.set(http::field::content_length, "4");
                                                       resp.set_empty_content(http::status::ok);
                                                   });
    ts.start_with_routes();

    auto client = ts.make_client();
    auto resp = UNWRAP(client->head("/head-test"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp[http::field::content_type] == "text/plain");
}

TEST_CASE("Client: 204 No Content response", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/empty-204",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_empty_content(http::status::no_content); });
    ts.start_with_routes();

    auto client = ts.make_client();
    auto resp = UNWRAP(client->get("/empty-204"));
    REQUIRE(resp.result() == http::status::no_content);
}

TEST_CASE("Client: 304 Not Modified response", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/not-modified",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_empty_content(http::status::not_modified); });
    ts.start_with_routes();

    auto client = ts.make_client();
    auto resp = UNWRAP(client->get("/not-modified"));
    REQUIRE(resp.result() == http::status::not_modified);
}

TEST_CASE("Client: download saves response body to file", "[client]")
{
    auto server_path = std::filesystem::temp_directory_path() / "httplib_dl_server.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "download test content\n";
    }

    auto dl_path = std::filesystem::temp_directory_path() / "httplib_dl_output.bin";

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/dl-file",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.start_with_routes();

    auto client = ts.make_client();
    auto resp = UNWRAP(client->download(http::verb::get, "/dl-file", dl_path));
    REQUIRE(resp.result() == http::status::ok);

    {
        std::ifstream dl_file(dl_path, std::ios::binary);
        REQUIRE(dl_file.is_open());
        std::string content((std::istreambuf_iterator<char>(dl_file)), std::istreambuf_iterator<char>());
        REQUIRE(content == "download test content\n");
    }

    std::filesystem::remove(server_path);
    std::filesystem::remove(dl_path);
}

TEST_CASE("Client: download randomized round-trip", "[client]")
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

        auto server_path = std::filesystem::temp_directory_path() / std::format("httplib_fuzz_srv_{}.bin", round);
        {
            std::ofstream f(server_path, std::ios::binary);
            f.write(sent.data(), sent.size());
        }

        auto dl_path = std::filesystem::temp_directory_path() / std::format("httplib_fuzz_dl_{}.bin", round);

        test_scaffold ts;
        ts.router().set_http_handler<http::verb::get>("/dl-fuzz",
                                                      [&](httplib::server::request&, httplib::server::response& resp)
                                                      { resp.set_file_content(server_path); });
        ts.start_with_routes();

        auto client = ts.make_client();
        auto resp = UNWRAP(client->download(http::verb::get, "/dl-fuzz", dl_path));
        REQUIRE(resp.result() == http::status::ok);

        {
            std::ifstream dl_file(dl_path, std::ios::binary);
            std::string received((std::istreambuf_iterator<char>(dl_file)), std::istreambuf_iterator<char>());
            REQUIRE(received == sent);
        }

        std::filesystem::remove(server_path);
        std::filesystem::remove(dl_path);
    }
}

TEST_CASE("Client: connection refused returns error", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    httplib::client::http_client bad_client(ts.executor(), ts.host(), 1);
    bad_client.set_timeout(std::chrono::seconds(2));

    auto resp = bad_client.get("/");
    REQUIRE_FALSE(resp.has_value());
}

TEST_CASE("Client: unreachable host returns error", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    httplib::client::http_client bad_client(ts.executor(), "192.0.2.1", 80);
    bad_client.set_timeout(std::chrono::seconds(2));

    auto resp = bad_client.get("/");
    REQUIRE_FALSE(resp.has_value());
}

TEST_CASE("Client: download with Range request", "[client]")
{
    auto server_path = std::filesystem::temp_directory_path() / "httplib_dl_range_server.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "0123456789";
    }

    auto dl_path = std::filesystem::temp_directory_path() / "httplib_dl_range_output.bin";

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/dl-range",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path, req.base()); });
    ts.start_with_routes();

    auto client = ts.make_client();
    auto headers = httplib::http::fields();
    headers.set(http::field::range, "bytes=0-4");

    auto resp = UNWRAP(client->download(http::verb::get, "/dl-range", dl_path, headers));
    REQUIRE(resp.result() == http::status::partial_content);

    {
        std::ifstream dl_file(dl_path, std::ios::binary);
        REQUIRE(dl_file.is_open());
        std::string content((std::istreambuf_iterator<char>(dl_file)), std::istreambuf_iterator<char>());
        REQUIRE(content == "01234");
    }

    std::filesystem::remove(server_path);
    std::filesystem::remove(dl_path);
}

TEST_CASE("Client: follows 302 redirect", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/redirect-me",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_redirect("/target", http::status::found); });
    ts.router().set_http_handler<http::verb::get>("/target",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_string_content("arrived"sv, "text/plain"sv); });
    ts.start_with_routes();

    auto client = ts.make_client();
    client->set_max_redirects(5);
    auto resp = UNWRAP(client->get("/redirect-me"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "arrived");
}

TEST_CASE("Client: redirect loop is limited", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/loop",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_redirect("/loop", http::status::found); });
    ts.start_with_routes();

    auto client = ts.make_client();
    client->set_max_redirects(3);
    auto resp = client->get("/loop");
    REQUIRE(resp.has_value());
    REQUIRE(resp->result() == http::status::found);
}

TEST_CASE("Client: redirect full URL creates new impl", "[client]")
{
    test_scaffold ts;

    ts.router().set_http_handler<http::verb::get>("/ext-redirect",
                                                  [](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      auto port = req.local_endpoint().port();
                                                      auto url = std::format("http://127.0.0.1:{}/target-page", port);
                                                      resp.set_redirect(url, http::status::found);
                                                  });
    ts.router().set_http_handler<http::verb::get>("/target-page",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_string_content("target-reached"sv, "text/plain"); });
    ts.start_with_routes();

    auto client = ts.make_client();
    client->set_max_redirects(1);
    auto resp = client->get("/ext-redirect");
    REQUIRE(resp.has_value());
    REQUIRE(resp->result() == http::status::ok);
    REQUIRE(as_string(resp.value()) == "target-reached");
}

TEST_CASE("Client: create via URL", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/url-test",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_string_content("url-ok"sv, "text/plain"); });
    ts.start_with_routes();

    auto url = std::format("http://{}:{}", ts.endpoint.address().to_string(), ts.endpoint.port());
    auto client = httplib::client::http_client(ts.ioc, url);
    client.set_timeout(std::chrono::seconds(5));
    auto resp = client.get("/url-test");
    REQUIRE(resp.has_value());
    REQUIRE(resp->result() == http::status::ok);
}
