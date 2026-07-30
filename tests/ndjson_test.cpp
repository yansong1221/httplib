#include "httplib/streaming/chunk_writer.hpp"
#include "httplib/client/client.hpp"
#include "httplib/streaming/ndjson_reader.hpp"
#include "httplib/streaming/ndjson_writer.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include "streaming/ndjson_reader_impl.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/system_executor.hpp>
#include <boost/asio/use_future.hpp>
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

namespace {

struct mock_chunk_reader : public httplib::chunk_reader
{
    std::vector<std::string> chunks;
    size_t idx = 0;

    httplib::net::awaitable<std::string_view> read_chunk() override
    {
        if (idx >= chunks.size())
            co_return std::string_view {};
        co_return chunks[idx++];
    }

    bool is_done() const override
    {
        return idx >= chunks.size();
    }
};

} // namespace

TEST_CASE("NDJSON: single line split across chunks", "[ndjson]")
{
    mock_chunk_reader mock;
    mock.chunks = {"{\"a\":", "1}\n"};

    httplib::streaming::ndjson_reader_impl reader(mock);

    auto f1 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::json::value> {
            co_return co_await reader.read();
        },
        boost::asio::use_future);
    f1.wait();
    REQUIRE(f1.get().at("a") == 1);
}

TEST_CASE("NDJSON: multiple lines in one chunk", "[ndjson]")
{
    mock_chunk_reader mock;
    mock.chunks = {"{\"a\":1}\n{\"b\":2}\n"};

    httplib::streaming::ndjson_reader_impl reader(mock);

    auto f1 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::json::value> {
            co_return co_await reader.read();
        },
        boost::asio::use_future);
    f1.wait();
    REQUIRE(f1.get().at("a") == 1);

    auto f2 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::json::value> {
            co_return co_await reader.read();
        },
        boost::asio::use_future);
    f2.wait();
    REQUIRE(f2.get().at("b") == 2);
}

TEST_CASE("NDJSON: mixed: partial line in first chunk, rest in second", "[ndjson]")
{
    mock_chunk_reader mock;
    mock.chunks = {"{\"x\":100}\n{\"y\":", "200}\n"};

    httplib::streaming::ndjson_reader_impl reader(mock);

    auto f1 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::json::value> {
            co_return co_await reader.read();
        },
        boost::asio::use_future);
    f1.wait();
    REQUIRE(f1.get().at("x") == 100);

    auto f2 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::json::value> {
            co_return co_await reader.read();
        },
        boost::asio::use_future);
    f2.wait();
    REQUIRE(f2.get().at("y") == 200);

    auto f3 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::json::value> {
            co_return co_await reader.read();
        },
        boost::asio::use_future);
    f3.wait();
    REQUIRE(f3.get().is_null());
}
