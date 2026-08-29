#include "common.hpp"
#include "httplib/server/chunk_writer.hpp"
#include "httplib/server/ndjson_writer.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/asio/co_spawn.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>

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
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
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
            auto resp
                = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/ndjson")));
            REQUIRE(resp.result() == http::status::ok);
            auto ndjson = resp.create_ndjson_reader();

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
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
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
            auto resp
                = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/ndjson")));
            REQUIRE(resp.result() == http::status::ok);
            auto ndjson = resp.create_ndjson_reader();

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
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
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
            auto resp
                = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/ndjson")));
            REQUIRE(resp.result() == http::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            REQUIRE(resp[http::field::content_type] == "application/x-ndjson");

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
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
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
            auto resp
                = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/ndjson")));
            REQUIRE(resp.result() == http::status::ok);
            auto ndjson = resp.create_ndjson_reader();

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
// NDJSON chunk-boundary tests (server sends split lines via chunk_writer)
// ===========================================================================

TEST_CASE("NDJSON: single line split across chunks", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.get_chunk_writer();
                    http::fields headers;
                    headers.set(http::field::content_type, "application/x-ndjson");
                    co_await cw->write_header(http::status::ok, headers, false);
                    co_await cw->write_body(net::buffer(std::string("{\"a\":1}")), true);
                    co_await cw->write_body(net::buffer(std::string("\n")), false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp
                = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/ndjson")));
            REQUIRE(resp.result() == http::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);
            REQUIRE(items.size() == 1);
            REQUIRE(items[0].at("a") == 1);

            co_return;
        });
}

TEST_CASE("NDJSON: multiple lines in one chunk", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.get_chunk_writer();
                    http::fields headers;
                    headers.set(http::field::content_type, "application/x-ndjson");
                    co_await cw->write_header(http::status::ok, headers, false);
                    co_await cw->write_body(net::buffer(std::string("{\"a\":1}\n{\"b\":2}\n")), false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp
                = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/ndjson")));
            REQUIRE(resp.result() == http::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);
            REQUIRE(items.size() == 2);
            REQUIRE(items[0].at("a") == 1);
            REQUIRE(items[1].at("b") == 2);

            co_return;
        });
}

TEST_CASE("NDJSON: partial line split across chunks", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.get_chunk_writer();
                    http::fields headers;
                    headers.set(http::field::content_type, "application/x-ndjson");
                    co_await cw->write_header(http::status::ok, headers, false);
                    co_await cw->write_body(net::buffer(std::string("{\"x\":100}\n{\"y\":")), true);
                    co_await cw->write_body(net::buffer(std::string("200}\n")), false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp
                = UNWRAP(co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/ndjson")));
            REQUIRE(resp.result() == http::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);
            REQUIRE(items.size() == 2);
            REQUIRE(items[0].at("x") == 100);
            REQUIRE(items[1].at("y") == 200);

            co_return;
        });
}

#ifdef HTTPLIB_ENABLED_COMPRESS
TEST_CASE("NDJSON: reader decodes gzip-compressed stream", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/ndjson-gzip",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("{\"msg\":\"hello\",\"n\":42}\n"sv, "application/json"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::http::fields headers;
            headers.set(http::field::accept_encoding, "gzip");
            auto resp = UNWRAP(co_await client.async_send_request_lazy(
                httplib::client::request(http::verb::get, "/ndjson-gzip", headers)));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp[http::field::content_encoding] == "gzip");

            auto ndjson = resp.create_ndjson_reader();
            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);
            REQUIRE(items.size() == 1);
            REQUIRE(items[0].at("msg") == "hello");
            REQUIRE(items[0].at("n") == 42);
            co_return;
        });
}
#endif
