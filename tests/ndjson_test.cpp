#include "httplib/client/client.hpp"
#include "httplib/client/read_session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include "httplib/streaming/chunk_writer.hpp"
#include "httplib/streaming/ndjson_reader.hpp"
#include "httplib/streaming/ndjson_writer.hpp"
#include "client/ndjson_reader_impl.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/system_executor.hpp>
#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>

using namespace std::string_view_literals;
namespace http = httplib::http;
namespace net = httplib::net;

namespace
{

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
            if (started_)
            {
                server.stop().wait();
                ioc.stop();
                if (thread.joinable())
                {
                    thread.join();
                }
            }
        }

        void
        start()
        {
            server.listen("127.0.0.1", 0);
            endpoint = server.local_endpoint();
            server.run();
            thread = std::thread([this] { ioc.run(); });
            started_ = true;

            client = std::make_unique<httplib::client::http_client>(ioc.get_executor(),
                                                                    endpoint.address().to_string(),
                                                                    endpoint.port());
            client->set_timeout(std::chrono::seconds(5));
        }

        auto&
        router()
        {
            return server.router();
        }
    };

#define UNWRAP(result)               \
    [&](auto&& r)                    \
    {                                \
        REQUIRE(r.has_value());      \
        return std::move(r).value(); \
    }(result)

} // namespace

TEST_CASE("NDJSON: server sends single line", "[ndjson]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/ndjson",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set_ndjson_write_handler(
                                                          [](httplib::ndjson_writer& w) -> net::awaitable<void>
                                                          {
                                                              boost::json::value v {
                                                                  { "msg", "hello" },
                                                                  {   "n",      42 }
                                                              };
                                                              co_await w.write(v);
                                                              co_await w.close();
                                                          });
                                                  });
    ts.start();

    std::vector<boost::json::value> items;

    boost::asio::co_spawn(ts.ioc,
                          [&]() -> net::awaitable<void>
                          {
                              auto ndjson = ts.client->create_ndjson_reader();

                              co_await ts.client->async_get("/ndjson");
                              auto ec = co_await ndjson->read_header();
                              REQUIRE(!ec);

                              while (!ndjson->is_done())
                              {
                                  auto result = co_await ndjson->read();
                if (result.has_error()) { break; }
                auto& val = result.value();
                                  if (val.is_null())
                                  {
                                      break;
                                  }
                                  items.push_back(std::move(val));
                              }
                          },
                          boost::asio::use_future)
        .get();

    REQUIRE(items.size() == 1);
    REQUIRE(items[0].at("msg") == "hello");
    REQUIRE(items[0].at("n") == 42);
}

TEST_CASE("NDJSON: server sends multiple lines", "[ndjson]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/ndjson",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set_ndjson_write_handler(
                                                          [](httplib::ndjson_writer& w) -> net::awaitable<void>
                                                          {
                                                              co_await w.write({
                                                                  { "i", 1 }
                                                              });
                                                              co_await w.write({
                                                                  { "i", 2 }
                                                              });
                                                              co_await w.write({
                                                                  { "i", 3 }
                                                              });
                                                              co_await w.close();
                                                          });
                                                  });
    ts.start();

    std::vector<boost::json::value> items;

    boost::asio::co_spawn(ts.ioc,
                          [&]() -> net::awaitable<void>
                          {
                              auto ndjson = ts.client->create_ndjson_reader();

                              co_await ts.client->async_get("/ndjson");
                              auto ec = co_await ndjson->read_header();
                              REQUIRE(!ec);

                              while (!ndjson->is_done())
                              {
                                  auto result = co_await ndjson->read();
                if (result.has_error()) { break; }
                auto& val = result.value();
                                  if (val.is_null())
                                  {
                                      break;
                                  }
                                  items.push_back(std::move(val));
                              }
                          },
                          boost::asio::use_future)
        .get();

    REQUIRE(items.size() == 3);
    REQUIRE(items[0].at("i") == 1);
    REQUIRE(items[1].at("i") == 2);
    REQUIRE(items[2].at("i") == 3);
}

TEST_CASE("NDJSON: Content-Type is application/x-ndjson", "[ndjson]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/ndjson",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set_ndjson_write_handler(
                                                          [](httplib::ndjson_writer& w) -> net::awaitable<void>
                                                          {
                                                              co_await w.write({
                                                                  { "x", 1 }
                                                              });
                                                              co_await w.close();
                                                          });
                                                  });
    ts.start();

    boost::asio::co_spawn(ts.ioc,
                          [&]() -> net::awaitable<void>
                          {
                              auto ndjson = ts.client->create_ndjson_reader();

                              co_await ts.client->async_get("/ndjson");
                              auto ec = co_await ndjson->read_header();
                              REQUIRE(!ec);

                              REQUIRE(ndjson->headers()[http::field::content_type] == "application/x-ndjson");
                          },
                          boost::asio::use_future)
        .get();
}

TEST_CASE("NDJSON: reader stops early", "[ndjson]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/ndjson",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set_ndjson_write_handler(
                                                          [](httplib::ndjson_writer& w) -> net::awaitable<void>
                                                          {
                                                              co_await w.write({
                                                                  { "i", 1 }
                                                              });
                                                              co_await w.write({
                                                                  { "i", 2 }
                                                              });
                                                              co_await w.write({
                                                                  { "i", 3 }
                                                              });
                                                              co_await w.close();
                                                          });
                                                  });
    ts.start();

    std::vector<boost::json::value> items;

    boost::asio::co_spawn(ts.ioc,
                          [&]() -> net::awaitable<void>
                          {
                              auto ndjson = ts.client->create_ndjson_reader();

                              co_await ts.client->async_get("/ndjson");
                              auto ec = co_await ndjson->read_header();
                              REQUIRE(!ec);

                              for (int i = 0; i < 2; ++i)
                              {
                                  auto result = co_await ndjson->read();
                if (result.has_error()) { break; }
                auto& val = result.value();
                                  if (val.is_null())
                                  {
                                      break;
                                  }
                                  items.push_back(std::move(val));
                              }
                          },
                          boost::asio::use_future)
        .get();

    REQUIRE(items.size() == 2);
}

namespace
{

    struct mock_read_session : public httplib::client::read_session
    {
        std::string data;
        size_t pos = 0;

        net::awaitable<boost::system::error_code>
        read_header() override
        {
            co_return boost::system::error_code {};
        }

        net::awaitable<boost::system::result<std::size_t>>
        read_body(net::mutable_buffer const& buffer) override
        {
            if (pos >= data.size())
            {
                co_return 0;
            }
            auto n = std::min(data.size() - pos, buffer.size());
            std::memcpy(buffer.data(), data.data() + pos, n);
            pos += n;
            co_return n;
        }

        http::status
        result() const override
        {
            return http::status::ok;
        }

        http::fields const&
        headers() const override
        {
            static http::fields f;
            return f;
        }

        bool
        is_header_done() const override
        {
            return true;
        }
    };

} // namespace

TEST_CASE("NDJSON: single line split across chunks", "[ndjson]")
{
    auto mock = std::make_shared<mock_read_session>();
    mock->data = "{\"a\":1}\n";

    httplib::client::ndjson_reader_impl reader(mock);

    auto f1 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>> { co_return co_await reader.read(); },
        boost::asio::use_future);
    f1.wait();
    REQUIRE(f1.get().value().at("a") == 1);
}

TEST_CASE("NDJSON: multiple lines in one chunk", "[ndjson]")
{
    auto mock = std::make_shared<mock_read_session>();
    mock->data = "{\"a\":1}\n{\"b\":2}\n";

    httplib::client::ndjson_reader_impl reader(mock);

    auto f1 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>> { co_return co_await reader.read(); },
        boost::asio::use_future);
    f1.wait();
    REQUIRE(f1.get().value().at("a") == 1);

    auto f2 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>> { co_return co_await reader.read(); },
        boost::asio::use_future);
    f2.wait();
    REQUIRE(f2.get().value().at("b") == 2);
}

TEST_CASE("NDJSON: mixed: partial line in first chunk, rest in second", "[ndjson]")
{
    auto mock = std::make_shared<mock_read_session>();
    mock->data = "{\"x\":100}\n{\"y\":200}\n";

    httplib::client::ndjson_reader_impl reader(mock);

    auto f1 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>> { co_return co_await reader.read(); },
        boost::asio::use_future);
    f1.wait();
    REQUIRE(f1.get().value().at("x") == 100);

    auto f2 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>> { co_return co_await reader.read(); },
        boost::asio::use_future);
    f2.wait();
    REQUIRE(f2.get().value().at("y") == 200);

    auto f3 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>> { co_return co_await reader.read(); },
        boost::asio::use_future);
    f3.wait();
    REQUIRE(f3.get().value().is_null());
}
