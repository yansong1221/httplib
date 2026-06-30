#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/beast/http/status.hpp>
#include <catch2/catch_test_macros.hpp>
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
    std::unique_ptr<httplib::client::http_client> client;

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

    void start()
    {
        server.listen("127.0.0.1", 0);
        endpoint = server.local_endpoint();
        server.run();
        thread = std::thread([this] { ioc.run(); });

        client = std::make_unique<httplib::client::http_client>(
            ioc.get_executor(), endpoint.address().to_string(), endpoint.port());
        client->set_timeout(std::chrono::seconds(5));
    }

    auto& router() { return server.router(); }
};

std::string as_string(const httplib::client::http_client::response& resp)
{
    return resp.body().as<httplib::body::string_body>();
}

void set_text(httplib::server::response& resp, std::string_view body,
              http::status status = http::status::ok)
{
    resp.set_string_content(body, "text/plain"sv, status);
}

#define UNWRAP(result)              \
    [&](auto&& r) {                 \
        REQUIRE(r.has_value());     \
        return std::move(r).value(); \
    }(result)

} // namespace

struct logging_aspect {
    std::string& log;

    bool before(httplib::server::request&, httplib::server::response&)
    {
        log += "before;";
        return true;
    }

    bool after(httplib::server::request&, httplib::server::response&)
    {
        log += "after;";
        return true;
    }
};

TEST_CASE("Aspect: before and after are called", "[aspect]")
{
    test_scaffold ts;
    std::string log;

    ts.router().set_http_handler<http::verb::get>(
        "/aspect",
        [](httplib::server::request&, httplib::server::response& resp) {
            set_text(resp, "handler");
        },
        logging_aspect{log});
    ts.start();

    auto resp = UNWRAP(ts.client->get("/aspect"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "handler");
    REQUIRE(log == "before;after;");
}

TEST_CASE("Aspect: before returning false stops handler chain", "[aspect]")
{
    struct blocking_aspect {
        bool before(httplib::server::request&, httplib::server::response& resp)
        {
            resp.set_string_content("blocked"sv, "text/plain"sv, http::status::forbidden);
            return false;
        }
        bool after(httplib::server::request&, httplib::server::response&) { return true; }
    };

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/blocked",
        [](httplib::server::request&, httplib::server::response& resp) {
            set_text(resp, "should-not-reach");
        },
        blocking_aspect{});
    ts.start();

    auto resp = UNWRAP(ts.client->get("/blocked"));
    REQUIRE(resp.result() == http::status::forbidden);
    REQUIRE(as_string(resp) == "blocked");
}

TEST_CASE("Aspect: multiple aspects chain in order", "[aspect]")
{
    std::string order;

    struct aspect_a {
        std::string& o;
        bool before(httplib::server::request&, httplib::server::response&)
        {
            o += "A.before;";
            return true;
        }
        bool after(httplib::server::request&, httplib::server::response&)
        {
            o += "A.after;";
            return true;
        }
    };

    struct aspect_b {
        std::string& o;
        bool before(httplib::server::request&, httplib::server::response&)
        {
            o += "B.before;";
            return true;
        }
        bool after(httplib::server::request&, httplib::server::response&)
        {
            o += "B.after;";
            return true;
        }
    };

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/multi-aspect",
        [](httplib::server::request&, httplib::server::response& resp) {
            set_text(resp, "ok");
        },
        aspect_a{order},
        aspect_b{order});
    ts.start();

    auto resp = UNWRAP(ts.client->get("/multi-aspect"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(order == "A.before;B.before;A.after;B.after;");
}

TEST_CASE("Aspect: 404 handler also supports aspects", "[aspect]")
{
    std::string log;

    test_scaffold ts;
    ts.router().set_http_not_found_handler(
        [](httplib::server::request&, httplib::server::response& resp) {
            set_text(resp, "custom-404", http::status::not_found);
        },
        logging_aspect{log});
    ts.start();

    auto resp = UNWRAP(ts.client->get("/non-existent"));
    REQUIRE(resp.result() == http::status::not_found);
    REQUIRE(as_string(resp) == "custom-404");
    REQUIRE(log == "before;after;");
}
