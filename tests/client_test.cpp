#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

using namespace std::string_view_literals;
namespace http  = httplib::http;
namespace net   = httplib::net;

namespace {

struct test_scaffold {
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
            thread.join();
    }

    void start_with_routes()
    {
        server.router().set_http_handler<http::verb::get>(
            "/echo",
            [](httplib::server::request& req, httplib::server::response& resp) {
                auto msg = std::string(req.query_params().at("msg"));
                resp.set_string_content(msg, "text/plain");
            });
        server.router().set_http_handler<http::verb::get>(
            "/slow",
            [](httplib::server::request&, httplib::server::response& resp) {
                resp.set_string_content("done"sv, "text/plain"sv);
            });

        server.listen("127.0.0.1", 0);
        endpoint = server.local_endpoint();
        server.run();
        thread = std::thread([this] { ioc.run(); });
    }

    std::unique_ptr<httplib::client::http_client> make_client()
    {
        auto c = std::make_unique<httplib::client::http_client>(
            ioc.get_executor(), endpoint.address().to_string(), endpoint.port());
        c->set_timeout(std::chrono::seconds(5));
        return c;
    }

    auto& router() { return server.router(); }

    std::string host() const { return endpoint.address().to_string(); }
    uint16_t port() const { return endpoint.port(); }

    net::any_io_executor executor() { return ioc.get_executor(); }
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

TEST_CASE("Client pool: acquire and use a connection", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    httplib::client::http_client_pool pool(ts.executor(), 4);
    {
        auto handle = pool.acquire(ts.host(), ts.port());
        REQUIRE(handle);

        auto params = httplib::html::query_params();
        params.add("msg", "hello-pool");
        auto resp = UNWRAP(handle->get("/echo", params));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp) == "hello-pool");
    }
}

TEST_CASE("Client pool: multiple acquires", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    httplib::client::http_client_pool pool(ts.executor(), 2);

    auto params = httplib::html::query_params();
    params.add("msg", "multi");

    {
        auto h1 = pool.acquire(ts.host(), ts.port());
        REQUIRE(h1);
        auto resp = UNWRAP(h1->get("/echo", params));
        REQUIRE(resp.result() == http::status::ok);
    }

    {
        auto h2 = pool.acquire(ts.host(), ts.port());
        REQUIRE(h2);
        auto resp = UNWRAP(h2->get("/echo", params));
        REQUIRE(resp.result() == http::status::ok);
    }
}

TEST_CASE("Client: close and is_open", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    auto client = ts.make_client();

    auto params = httplib::html::query_params();
    params.add("msg", "hello");
    auto resp = UNWRAP(client->get("/echo", params));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(client->is_open());

    client->close();
    REQUIRE_FALSE(client->is_open());
}

TEST_CASE("Client: custom chunk handler", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/chunked",
        [](httplib::server::request&, httplib::server::response& resp) {
            auto idx = std::make_shared<int>(0);
            resp.set_stream_content(
                [idx](httplib::beast::flat_buffer& buffer,
                      boost::system::error_code&) -> net::awaitable<bool> {
                    constexpr std::string_view parts[] = {"part-", "one-", "two"};
                    if (*idx >= 3)
                        co_return false;

                    auto p = parts[*idx];
                    ++(*idx);
                    buffer.commit(
                        net::buffer_copy(buffer.prepare(p.size()), net::buffer(p)));
                    co_return *idx < 3;
                },
                "text/plain");
        });
    ts.start_with_routes();

    std::vector<std::string> chunks;
    auto client = ts.make_client();
    client->set_chunk_handler(
        [&](std::string_view data, boost::system::error_code&) {
            chunks.emplace_back(data);
        });

    auto resp = UNWRAP(client->get("/chunked"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(chunks.size() == 3);
    REQUIRE(chunks[0] == "part-");
    REQUIRE(chunks[1] == "one-");
    REQUIRE(chunks[2] == "two");

    client->set_chunk_handler(nullptr);
}

TEST_CASE("Client host and port", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    auto client = ts.make_client();
    REQUIRE(client->host() == ts.host());
    REQUIRE(client->port() == ts.port());
    REQUIRE_FALSE(client->is_use_ssl());
}

TEST_CASE("Client: timeout_policy step", "[client]")
{
    test_scaffold ts;
    ts.start_with_routes();

    auto client = ts.make_client();
    client->set_timeout_policy(httplib::client::http_client::timeout_policy::step);
    client->set_timeout(std::chrono::seconds(5));

    auto params = httplib::html::query_params();
    params.add("msg", "step-timeout");
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
    params.add("msg", "never-timeout");
    auto resp = UNWRAP(client->get("/echo", params));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Client: HEAD request", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::head>(
        "/head-test",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set(http::field::content_type, "text/plain");
            resp.set(http::field::content_length, "4");
            resp.set_empty_content(http::status::ok);
        });
    ts.start_with_routes();

    auto client = ts.make_client();
    auto resp   = UNWRAP(client->head("/head-test"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp[http::field::content_type] == "text/plain");
}

TEST_CASE("Client: 204 No Content response", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/empty-204",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_empty_content(http::status::no_content);
        });
    ts.start_with_routes();

    auto client = ts.make_client();
    auto resp   = UNWRAP(client->get("/empty-204"));
    REQUIRE(resp.result() == http::status::no_content);
}

TEST_CASE("Client: 304 Not Modified response", "[client]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/not-modified",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_empty_content(http::status::not_modified);
        });
    ts.start_with_routes();

    auto client = ts.make_client();
    auto resp   = UNWRAP(client->get("/not-modified"));
    REQUIRE(resp.result() == http::status::not_modified);
}
