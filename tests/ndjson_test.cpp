#include "client/ndjson_reader_impl.hpp"
#include "common.hpp"
#include "httplib/server/ndjson_writer.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/asio/co_spawn.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <chrono>

namespace net = httplib::net;
namespace http = httplib::http;
using test_common::run;
using test_common::setup_logger;

// ===========================================================================
// NDJSON integration tests
// ===========================================================================

TEST_CASE("NDJSON: server sends single line", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ndjson",
                [](httplib::server::request&,
                   httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto w = resp.create_ndjson_writer();
                    co_await w->begin();
                    boost::json::value v {
                        { "msg", "hello" },
                        {   "n",      42 }
                    };
                    co_await w->write(v, false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto ndjson = client.create_ndjson_reader();

            co_await client.async_get("/ndjson");
            auto ec = co_await ndjson->read_header();
            REQUIRE(!ec);

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);

            REQUIRE(items.size() == 1);
            REQUIRE(items[0].at("msg") == "hello");
            REQUIRE(items[0].at("n") == 42);

            co_return;
        });
}

TEST_CASE("NDJSON: server sends multiple lines", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ndjson",
                [](httplib::server::request&,
                   httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto w = resp.create_ndjson_writer();
                    co_await w->begin();
                    co_await w->write(
                        {
                            { "i", 1 }
                    },
                        true);
                    co_await w->write(
                        {
                            { "i", 2 }
                    },
                        true);
                    co_await w->write(
                        {
                            { "i", 3 }
                    },
                        false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto ndjson = client.create_ndjson_reader();

            co_await client.async_get("/ndjson");
            auto ec = co_await ndjson->read_header();
            REQUIRE(!ec);

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);

            REQUIRE(items.size() == 3);
            REQUIRE(items[0].at("i") == 1);
            REQUIRE(items[1].at("i") == 2);
            REQUIRE(items[2].at("i") == 3);

            co_return;
        });
}

TEST_CASE("NDJSON: Content-Type is application/x-ndjson", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ndjson",
                [](httplib::server::request&,
                   httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto w = resp.create_ndjson_writer();
                    co_await w->begin();
                    co_await w->write(
                        {
                            { "x", 1 }
                    },
                        false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto ndjson = client.create_ndjson_reader();

            co_await client.async_get("/ndjson");
            auto ec = co_await ndjson->read_header();
            REQUIRE(!ec);

            REQUIRE(ndjson->headers()[http::field::content_type] == "application/x-ndjson");

            co_return;
        });
}

TEST_CASE("NDJSON: reader stops early", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ndjson",
                [](httplib::server::request&,
                   httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto w = resp.create_ndjson_writer();
                    co_await w->begin();
                    co_await w->write(
                        {
                            { "i", 1 }
                    },
                        true);
                    co_await w->write(
                        {
                            { "i", 2 }
                    },
                        true);
                    co_await w->write(
                        {
                            { "i", 3 }
                    },
                        false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto ndjson = client.create_ndjson_reader();

            co_await client.async_get("/ndjson");
            auto ec = co_await ndjson->read_header();
            REQUIRE(!ec);

            std::vector<boost::json::value> items;
            for (int i = 0; i < 2; ++i)
            {
                auto result = co_await ndjson->read();
                if (result.has_error())
                {
                    break;
                }
                auto& val = result.value();
                if (val.is_null())
                {
                    break;
                }
                items.push_back(std::move(val));
            }

            REQUIRE(items.size() == 2);

            co_return;
        });
}

// ===========================================================================
// NDJSON unit tests with mock_read_session
// ===========================================================================

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

        bool
        is_body_done() const override
        {
            return pos >= data.size();
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
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>>
        { co_return co_await reader.read(); },
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
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>>
        { co_return co_await reader.read(); },
        boost::asio::use_future);
    f1.wait();
    REQUIRE(f1.get().value().at("a") == 1);

    auto f2 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>>
        { co_return co_await reader.read(); },
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
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>>
        { co_return co_await reader.read(); },
        boost::asio::use_future);
    f1.wait();
    REQUIRE(f1.get().value().at("x") == 100);

    auto f2 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>>
        { co_return co_await reader.read(); },
        boost::asio::use_future);
    f2.wait();
    REQUIRE(f2.get().value().at("y") == 200);

    auto f3 = boost::asio::co_spawn(
        boost::asio::system_executor {},
        [&]() -> httplib::net::awaitable<boost::system::result<boost::json::value>>
        { co_return co_await reader.read(); },
        boost::asio::use_future);
    f3.wait();
    REQUIRE(f3.get().value().is_null());
}
