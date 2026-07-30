#include "httplib/chunk_writer.hpp"
#include "httplib/client/client.hpp"
#include "httplib/ndjson_reader.hpp"
#include "httplib/ndjson_writer.hpp"
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
#include <vector>

using namespace std::string_view_literals;
namespace http = httplib::http;
namespace net  = httplib::net;

namespace {

struct test_scaffold
{
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
        thread   = std::thread([this] { ioc.run(); });
        started_ = true;

        client = std::make_unique<httplib::client::http_client>(
            ioc.get_executor(), endpoint.address().to_string(), endpoint.port());
        client->set_timeout(std::chrono::seconds(5));
    }

    auto& router() { return server.router(); }
};

#define UNWRAP(result)                                                                             \
    [&](auto&& r) {                                                                                \
        REQUIRE(r.has_value());                                                                    \
        return std::move(r).value();                                                               \
    }(result)

} // namespace

TEST_CASE("NDJSON: server sends single line", "[ndjson]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/ndjson",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_ndjson_write_handler(
                [](httplib::ndjson_writer& w) -> net::awaitable<void> {
                    boost::json::value v {{"msg", "hello"}, {"n", 42}};
                    co_await w.write(v);
                    co_await w.close();
                });
        });
    ts.start();

    std::vector<boost::json::value> items;
    ts.client->set_ndjson_read_handler(
        [&](httplib::ndjson_reader& reader) -> net::awaitable<void> {
            while (!reader.is_done()) {
                auto val = co_await reader.read();
                if (val.is_null())
                    break;
                items.push_back(std::move(val));
            }
        });

    auto resp = UNWRAP(ts.client->get("/ndjson"));
    REQUIRE(resp.result() == http::status::ok);

    REQUIRE(items.size() == 1);
    REQUIRE(items[0].at("msg") == "hello");
    REQUIRE(items[0].at("n") == 42);
}

TEST_CASE("NDJSON: server sends multiple lines", "[ndjson]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/ndjson",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_ndjson_write_handler(
                [](httplib::ndjson_writer& w) -> net::awaitable<void> {
                    co_await w.write({{"i", 1}});
                    co_await w.write({{"i", 2}});
                    co_await w.write({{"i", 3}});
                    co_await w.close();
                });
        });
    ts.start();

    std::vector<boost::json::value> items;
    ts.client->set_ndjson_read_handler(
        [&](httplib::ndjson_reader& reader) -> net::awaitable<void> {
            while (!reader.is_done()) {
                auto val = co_await reader.read();
                if (val.is_null())
                    break;
                items.push_back(std::move(val));
            }
        });

    auto resp = UNWRAP(ts.client->get("/ndjson"));
    REQUIRE(resp.result() == http::status::ok);

    REQUIRE(items.size() == 3);
    REQUIRE(items[0].at("i") == 1);
    REQUIRE(items[1].at("i") == 2);
    REQUIRE(items[2].at("i") == 3);
}

TEST_CASE("NDJSON: Content-Type is application/x-ndjson", "[ndjson]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/ndjson",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_ndjson_write_handler(
                [](httplib::ndjson_writer& w) -> net::awaitable<void> {
                    co_await w.write({{"x", 1}});
                    co_await w.close();
                });
        });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/ndjson"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp[http::field::content_type] == "application/x-ndjson");
}

TEST_CASE("NDJSON: reader stops early", "[ndjson]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/ndjson",
        [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_ndjson_write_handler(
                [](httplib::ndjson_writer& w) -> net::awaitable<void> {
                    co_await w.write({{"i", 1}});
                    co_await w.write({{"i", 2}});
                    co_await w.write({{"i", 3}});
                    co_await w.close();
                });
        });
    ts.start();

    std::vector<boost::json::value> items;
    ts.client->set_ndjson_read_handler(
        [&](httplib::ndjson_reader& reader) -> net::awaitable<void> {
            for (int i = 0; i < 2; ++i) {
                auto val = co_await reader.read();
                if (val.is_null())
                    break;
                items.push_back(std::move(val));
            }
        });

    auto resp = UNWRAP(ts.client->get("/ndjson"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(items.size() == 2);
}
