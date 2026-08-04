#include "common.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/sse_writer.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

namespace
{
    using test_common::test_scaffold;
} // namespace

TEST_CASE("SSE: server sends single event", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto sse = resp.create_sse_writer();
            co_await sse->begin();
            co_await sse->send_event("hello world", {}, {}, false);
        });
    ts.start();

    std::vector<httplib::client::sse_reader::sse_event> events;

    boost::asio::co_spawn(
        ts.executor(),
        [&]() -> net::awaitable<void>
        {
            auto sse = ts.client->create_sse_reader();

            co_await ts.client->async_get("/events");
            auto ec = co_await sse->read_header();
            REQUIRE(!ec);

            co_await collect_sse_events(*sse, events);
        },
        boost::asio::use_future)
        .get();

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "hello world");
}

TEST_CASE("SSE: server sends multiple events", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto sse = resp.create_sse_writer();
            co_await sse->begin();
            co_await sse->send_event("first", {}, {}, true);
            co_await sse->send_event("second", {}, {}, true);
            co_await sse->send_event("third", {}, {}, false);
        });
    ts.start();

    std::vector<httplib::client::sse_reader::sse_event> events;

    boost::asio::co_spawn(
        ts.executor(),
        [&]() -> net::awaitable<void>
        {
            auto sse = ts.client->create_sse_reader();

            co_await ts.client->async_get("/events");
            auto ec = co_await sse->read_header();
            REQUIRE(!ec);

            co_await collect_sse_events(*sse, events);
        },
        boost::asio::use_future)
        .get();

    REQUIRE(events.size() == 3);
    REQUIRE(events[0].data == "first");
    REQUIRE(events[1].data == "second");
    REQUIRE(events[2].data == "third");
}

TEST_CASE("SSE: event with id and type", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto sse = resp.create_sse_writer();
            co_await sse->begin();
            co_await sse->send_event("payload", "custom_type", "42", false);
        });
    ts.start();

    std::vector<httplib::client::sse_reader::sse_event> events;

    boost::asio::co_spawn(
        ts.executor(),
        [&]() -> net::awaitable<void>
        {
            auto sse = ts.client->create_sse_reader();

            co_await ts.client->async_get("/events");
            auto ec = co_await sse->read_header();
            REQUIRE(!ec);

            co_await collect_sse_events(*sse, events);
        },
        boost::asio::use_future)
        .get();

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].id == "42");
    REQUIRE(events[0].event == "custom_type");
    REQUIRE(events[0].data == "payload");
}

TEST_CASE("SSE: retry interval", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto sse = resp.create_sse_writer();
            co_await sse->begin();
            co_await sse->send_retry(std::chrono::milliseconds(3000), false);
        });
    ts.start();

    std::vector<httplib::client::sse_reader::sse_event> events;

    boost::asio::co_spawn(
        ts.executor(),
        [&]() -> net::awaitable<void>
        {
            auto sse = ts.client->create_sse_reader();

            co_await ts.client->async_get("/events");
            auto ec = co_await sse->read_header();
            REQUIRE(!ec);

            co_await collect_sse_events(*sse, events);
        },
        boost::asio::use_future)
        .get();

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].retry == std::chrono::milliseconds(3000));
    REQUIRE(events[0].data.empty());
}

TEST_CASE("SSE: comment is ignored", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto sse = resp.create_sse_writer();
            co_await sse->begin();
            co_await sse->send_comment("keep-alive", true);
            co_await sse->send_event("real data", {}, {}, false);
        });
    ts.start();

    std::vector<httplib::client::sse_reader::sse_event> events;

    boost::asio::co_spawn(
        ts.executor(),
        [&]() -> net::awaitable<void>
        {
            auto sse = ts.client->create_sse_reader();

            co_await ts.client->async_get("/events");
            auto ec = co_await sse->read_header();
            REQUIRE(!ec);

            co_await collect_sse_events(*sse, events);
        },
        boost::asio::use_future)
        .get();

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "real data");
}

TEST_CASE("SSE: Content-Type is text/event-stream", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto sse = resp.create_sse_writer();
            co_await sse->begin();
            co_await sse->send_event("test", {}, {}, false);
        });
    ts.start();

    boost::asio::co_spawn(
        ts.executor(),
        [&]() -> net::awaitable<void>
        {
            auto sse = ts.client->create_sse_reader();

            co_await ts.client->async_get("/events");
            auto ec = co_await sse->read_header();
            REQUIRE(!ec);

            REQUIRE(sse->headers()[http::field::content_type] == "text/event-stream");
            REQUIRE(sse->headers()[http::field::cache_control] == "no-cache");
        },
        boost::asio::use_future)
        .get();
}

TEST_CASE("SSE: client can stop receiving by returning false", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto sse = resp.create_sse_writer();
            co_await sse->begin();
            co_await sse->send_event("first", {}, {}, true);
            co_await sse->send_event("second", {}, {}, true);
            co_await sse->send_event("third", {}, {}, false);
        });
    ts.start();

    std::vector<httplib::client::sse_reader::sse_event> events;

    boost::asio::co_spawn(
        ts.executor(),
        [&]() -> net::awaitable<void>
        {
            auto sse = ts.client->create_sse_reader();

            co_await ts.client->async_get("/events");
            auto ec = co_await sse->read_header();
            REQUIRE(!ec);

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
        },
        boost::asio::use_future)
        .get();

    REQUIRE(events.size() == 2);
    REQUIRE(events[0].data == "first");
    REQUIRE(events[1].data == "second");
}

TEST_CASE("SSE: multi-line data", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto sse = resp.create_sse_writer();
            co_await sse->begin();
            co_await sse->send_event("line1\nline2\nline3", {}, {}, false);
        });
    ts.start();

    std::vector<httplib::client::sse_reader::sse_event> events;

    boost::asio::co_spawn(
        ts.executor(),
        [&]() -> net::awaitable<void>
        {
            auto sse = ts.client->create_sse_reader();

            co_await ts.client->async_get("/events");
            auto ec = co_await sse->read_header();
            REQUIRE(!ec);

            co_await collect_sse_events(*sse, events);
        },
        boost::asio::use_future)
        .get();

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "line1\nline2\nline3");
}
