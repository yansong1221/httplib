#include "httplib/chunk_writer.hpp"
#include "httplib/client/client.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include "httplib/sse_reader.hpp"
#include "httplib/sse_writer.hpp"
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

    test_scaffold()
        : server(ioc)
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

    void start_with_long_timeout()
    {
        server.listen("127.0.0.1", 0);
        endpoint = server.local_endpoint();
        server.run();
        thread   = std::thread([this] { ioc.run(); });
        started_ = true;

        client = std::make_unique<httplib::client::http_client>(
            ioc.get_executor(), endpoint.address().to_string(), endpoint.port());
        client->set_timeout(std::chrono::seconds(30));
    }

    auto& router() { return server.router(); }
};

#define UNWRAP(result)                                                                             \
    [&](auto&& r) {                                                                                \
        REQUIRE(r.has_value());                                                                    \
        return std::move(r).value();                                                               \
    }(result)

} // namespace

TEST_CASE("SSE: server sends single event", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events", [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_sse_write_handler([](httplib::sse_writer& sse) -> net::awaitable<void> {
                co_await sse.send_event("hello world");
                co_await sse.close();
            });
        });
    ts.start();

    std::vector<httplib::sse_event> events;
    ts.client->set_sse_read_handler([&](httplib::sse_reader& reader) -> net::awaitable<void> {
        while (!reader.is_done()) {
            auto ev = co_await reader.read_event();
            if (ev.data.empty() && ev.id.empty() && ev.event.empty() &&
                ev.retry == std::chrono::milliseconds {0})
                break;
            events.push_back(std::move(ev));
        }
    });

    auto resp = UNWRAP(ts.client->get("/events"));
    REQUIRE(resp.result() == http::status::ok);

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "hello world");
}

TEST_CASE("SSE: server sends multiple events", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events", [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_sse_write_handler([](httplib::sse_writer& sse) -> net::awaitable<void> {
                co_await sse.send_event("first");
                co_await sse.send_event("second");
                co_await sse.send_event("third");
                co_await sse.close();
            });
        });
    ts.start();

    std::vector<httplib::sse_event> events;
    ts.client->set_sse_read_handler([&](httplib::sse_reader& reader) -> net::awaitable<void> {
        while (!reader.is_done()) {
            auto ev = co_await reader.read_event();
            if (ev.data.empty() && ev.id.empty() && ev.event.empty() &&
                ev.retry == std::chrono::milliseconds {0})
                break;
            events.push_back(std::move(ev));
        }
    });

    auto resp = UNWRAP(ts.client->get("/events"));
    REQUIRE(resp.result() == http::status::ok);

    REQUIRE(events.size() == 3);
    REQUIRE(events[0].data == "first");
    REQUIRE(events[1].data == "second");
    REQUIRE(events[2].data == "third");
}

TEST_CASE("SSE: event with id and type", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events", [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_sse_write_handler([](httplib::sse_writer& sse) -> net::awaitable<void> {
                co_await sse.send_event("payload", "custom_type", "42");
                co_await sse.close();
            });
        });
    ts.start();

    std::vector<httplib::sse_event> events;
    ts.client->set_sse_read_handler([&](httplib::sse_reader& reader) -> net::awaitable<void> {
        while (!reader.is_done()) {
            auto ev = co_await reader.read_event();
            if (ev.data.empty() && ev.id.empty() && ev.event.empty() &&
                ev.retry == std::chrono::milliseconds {0})
                break;
            events.push_back(std::move(ev));
        }
    });

    auto resp = UNWRAP(ts.client->get("/events"));
    REQUIRE(resp.result() == http::status::ok);

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].id == "42");
    REQUIRE(events[0].event == "custom_type");
    REQUIRE(events[0].data == "payload");
}

TEST_CASE("SSE: retry interval", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events", [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_sse_write_handler([](httplib::sse_writer& sse) -> net::awaitable<void> {
                co_await sse.send_retry(std::chrono::milliseconds(3000));
                co_await sse.close();
            });
        });
    ts.start();

    std::vector<httplib::sse_event> events;
    ts.client->set_sse_read_handler([&](httplib::sse_reader& reader) -> net::awaitable<void> {
        while (!reader.is_done()) {
            auto ev = co_await reader.read_event();
            if (ev.data.empty() && ev.id.empty() && ev.event.empty() &&
                ev.retry == std::chrono::milliseconds {0})
                break;
            events.push_back(std::move(ev));
        }
    });

    auto resp = UNWRAP(ts.client->get("/events"));
    REQUIRE(resp.result() == http::status::ok);

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].retry == std::chrono::milliseconds(3000));
    REQUIRE(events[0].data.empty());
}

TEST_CASE("SSE: comment is ignored", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events", [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_sse_write_handler([](httplib::sse_writer& sse) -> net::awaitable<void> {
                co_await sse.send_comment("keep-alive");
                co_await sse.send_event("real data");
                co_await sse.close();
            });
        });
    ts.start();

    std::vector<httplib::sse_event> events;
    ts.client->set_sse_read_handler([&](httplib::sse_reader& reader) -> net::awaitable<void> {
        while (!reader.is_done()) {
            auto ev = co_await reader.read_event();
            if (ev.data.empty() && ev.id.empty() && ev.event.empty() &&
                ev.retry == std::chrono::milliseconds {0})
                break;
            events.push_back(std::move(ev));
        }
    });

    auto resp = UNWRAP(ts.client->get("/events"));
    REQUIRE(resp.result() == http::status::ok);

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "real data");
}

TEST_CASE("SSE: Content-Type is text/event-stream", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events", [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_sse_write_handler([](httplib::sse_writer& sse) -> net::awaitable<void> {
                co_await sse.send_event("test");
                co_await sse.close();
            });
        });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/events"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp[http::field::content_type] == "text/event-stream");
    REQUIRE(resp[http::field::cache_control] == "no-cache");
}

TEST_CASE("SSE: client can stop receiving by returning false", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events", [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_sse_write_handler([](httplib::sse_writer& sse) -> net::awaitable<void> {
                co_await sse.send_event("first");
                co_await sse.send_event("second");
                co_await sse.send_event("third");
                co_await sse.close();
            });
        });
    ts.start();

    std::vector<httplib::sse_event> events;
    ts.client->set_sse_read_handler([&](httplib::sse_reader& reader) -> net::awaitable<void> {
        while (!reader.is_done()) {
            auto ev = co_await reader.read_event();
            if (ev.data.empty() && ev.id.empty())
                break;
            events.push_back(std::move(ev));
            if (events.size() >= 2)
                co_return;
        }
    });

    auto resp = UNWRAP(ts.client->get("/events"));
    REQUIRE(resp.result() == http::status::ok);

    REQUIRE(events.size() == 2);
    REQUIRE(events[0].data == "first");
    REQUIRE(events[1].data == "second");
}

TEST_CASE("SSE: multi-line data", "[sse]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/events", [](httplib::server::request&, httplib::server::response& resp) {
            resp.set_sse_write_handler([](httplib::sse_writer& sse) -> net::awaitable<void> {
                co_await sse.send_event("line1\nline2\nline3");
                co_await sse.close();
            });
        });
    ts.start();

    std::vector<httplib::sse_event> events;
    ts.client->set_sse_read_handler([&](httplib::sse_reader& reader) -> net::awaitable<void> {
        while (!reader.is_done()) {
            auto ev = co_await reader.read_event();
            if (ev.data.empty() && ev.id.empty() && ev.event.empty() &&
                ev.retry == std::chrono::milliseconds {0})
                break;
            events.push_back(std::move(ev));
        }
    });

    auto resp = UNWRAP(ts.client->get("/events"));
    REQUIRE(resp.result() == http::status::ok);

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "line1\nline2\nline3");
}
