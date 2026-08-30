#include "common.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/sse_writer.hpp"
#include <catch2/catch_test_macros.hpp>

namespace net = httplib::net;
using test_common::run;
using test_common::setup_logger;

// ===========================================================================
// SSE basic
// ===========================================================================

TEST_CASE("SSE: server sends single event", "[sse]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/events",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto sse = resp.create_sse_writer();
                    co_await sse->begin();
                    co_await sse->send_event("hello world", {}, {}, false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::vector<httplib::client::sse_reader::sse_event> events;

            auto resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/events"), httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            auto sse = resp.create_sse_reader();

            co_await collect_sse_events(*sse, events);

            REQUIRE(events.size() == 1);
            REQUIRE(events[0].data == "hello world");
            co_return;
        });
}

TEST_CASE("SSE: server sends multiple events", "[sse]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/events",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto sse = resp.create_sse_writer();
                    co_await sse->begin();
                    co_await sse->send_event("first", {}, {}, true);
                    co_await sse->send_event("second", {}, {}, true);
                    co_await sse->send_event("third", {}, {}, false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::vector<httplib::client::sse_reader::sse_event> events;

            auto resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/events"), httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            auto sse = resp.create_sse_reader();

            co_await collect_sse_events(*sse, events);

            REQUIRE(events.size() == 3);
            REQUIRE(events[0].data == "first");
            REQUIRE(events[1].data == "second");
            REQUIRE(events[2].data == "third");
            co_return;
        });
}

TEST_CASE("SSE: event with id and type", "[sse]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/events",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto sse = resp.create_sse_writer();
                    co_await sse->begin();
                    co_await sse->send_event("payload", "custom_type", "42", false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::vector<httplib::client::sse_reader::sse_event> events;

            auto resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/events"), httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            auto sse = resp.create_sse_reader();

            co_await collect_sse_events(*sse, events);

            REQUIRE(events.size() == 1);
            REQUIRE(events[0].id == "42");
            REQUIRE(events[0].event == "custom_type");
            REQUIRE(events[0].data == "payload");
            co_return;
        });
}

TEST_CASE("SSE: retry interval", "[sse]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/events",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto sse = resp.create_sse_writer();
                    co_await sse->begin();
                    co_await sse->send_retry(std::chrono::milliseconds(3000), false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::vector<httplib::client::sse_reader::sse_event> events;

            auto resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/events"), httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            auto sse = resp.create_sse_reader();

            co_await collect_sse_events(*sse, events);

            REQUIRE(events.size() == 1);
            REQUIRE(events[0].retry == std::chrono::milliseconds(3000));
            REQUIRE(events[0].data.empty());
            co_return;
        });
}

TEST_CASE("SSE: comment is ignored", "[sse]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/events",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto sse = resp.create_sse_writer();
                    co_await sse->begin();
                    co_await sse->send_comment("keep-alive", true);
                    co_await sse->send_event("real data", {}, {}, false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::vector<httplib::client::sse_reader::sse_event> events;

            auto resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/events"), httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            auto sse = resp.create_sse_reader();

            co_await collect_sse_events(*sse, events);

            REQUIRE(events.size() == 1);
            REQUIRE(events[0].data == "real data");
            co_return;
        });
}

// ===========================================================================
// SSE headers
// ===========================================================================

TEST_CASE("SSE: Content-Type is text/event-stream", "[sse]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/events",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto sse = resp.create_sse_writer();
                    co_await sse->begin();
                    co_await sse->send_event("test", {}, {}, false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/events"), httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            auto sse = resp.create_sse_reader();

            REQUIRE(resp[http::field::content_type] == "text/event-stream");
            REQUIRE(resp[http::field::cache_control] == "no-cache");
            co_return;
        });
}

// ===========================================================================
// SSE client control
// ===========================================================================

TEST_CASE("SSE: client can stop receiving by returning false", "[sse]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/events",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto sse = resp.create_sse_writer();
                    co_await sse->begin();
                    co_await sse->send_event("first", {}, {}, true);
                    co_await sse->send_event("second", {}, {}, true);
                    co_await sse->send_event("third", {}, {}, false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::vector<httplib::client::sse_reader::sse_event> events;

            auto resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/events"), httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            auto sse = resp.create_sse_reader();

            while (!sse->is_done())
            {
                auto result = co_await sse->read_event();
                if (result.has_error())
                {
                    break;
                }
                auto& ev = result.value();
                if (ev.data.empty() && ev.id.empty())
                {
                    break;
                }
                events.push_back(std::move(ev));
                if (events.size() >= 2)
                {
                    co_return;
                }
            }

            co_return;
        });
}

// ===========================================================================
// SSE data
// ===========================================================================

TEST_CASE("SSE: multi-line data", "[sse]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/events",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto sse = resp.create_sse_writer();
                    co_await sse->begin();
                    co_await sse->send_event("line1\nline2\nline3", {}, {}, false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::vector<httplib::client::sse_reader::sse_event> events;

            auto resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/events"), httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            auto sse = resp.create_sse_reader();

            co_await collect_sse_events(*sse, events);

            REQUIRE(events.size() == 1);
            REQUIRE(events[0].data == "line1\nline2\nline3");
            co_return;
        });
}

// ===========================================================================
// SSE lazy response
// ===========================================================================

TEST_CASE("SSE: lazy response reader", "[sse]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/events",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto sse = resp.create_sse_writer();
                    co_await sse->begin();
                    co_await sse->send_event("lazy-event", {}, {}, false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::vector<httplib::client::sse_reader::sse_event> events;

            auto resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/events"), httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);

            auto sse = resp.create_sse_reader();
            co_await collect_sse_events(*sse, events);

            REQUIRE(events.size() == 1);
            REQUIRE(events[0].data == "lazy-event");
        });
}

#ifdef HTTPLIB_ENABLED_COMPRESS
TEST_CASE("SSE: reader decodes gzip-compressed event stream", "[sse]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/events-gzip",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("data: hello gzip\n\n"sv, "text/event-stream"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::http::fields headers;
            headers.set(http::field::accept_encoding, "gzip");
            auto resp = UNWRAP(co_await client.async_send_request(
                httplib::client::request(http::verb::get, "/events-gzip", headers),
                httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp[http::field::content_encoding] == "gzip");

            std::vector<httplib::client::sse_reader::sse_event> events;
            auto sse = resp.create_sse_reader();
            co_await collect_sse_events(*sse, events);
            REQUIRE(events.size() == 1);
            REQUIRE(events[0].data == "hello gzip");
            co_return;
        });
}
#endif
