#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <regex>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

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

        std::string
        host() const
        {
            return endpoint.address().to_string();
        }
        uint16_t
        port() const
        {
            return endpoint.port();
        }
    };

    std::string
    as_string(httplib::client::http_client::response const& resp)
    {
        return resp.body().as<httplib::body::string_body>();
    }

    void
    set_text(httplib::server::response& resp,
             std::string_view body,
             httplib::http::status status = httplib::http::status::ok)
    {
        resp.set_string_content(body, "text/plain"sv, status);
    }

#define UNWRAP(result)               \
    [&](auto&& r)                    \
    {                                \
        REQUIRE(r.has_value());      \
        return std::move(r).value(); \
    }(result)

} // namespace

TEST_CASE("Router: named path parameter :name", "[router]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/users/:id",
                                                  [](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      auto id = req.path_param("id");
                                                      REQUIRE(id == "42");
                                                      set_text(resp, "user-" + std::string(id));
                                                  });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/users/42"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "user-42");
}

TEST_CASE("Router: regex path parameter {name:pattern}", "[router]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/regex/{id:^\\d+$}",
                                                  [](httplib::server::request& req, httplib::server::response& resp)
                                                  { set_text(resp, "regex-" + std::string(req.path_param("id"))); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/regex/12345"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "regex-12345");
}

TEST_CASE("Router: regex path parameter rejects non-matching input", "[router]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/regex/{id:^\\d+$}",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { set_text(resp, "should-not-match"); });
    ts.router().set_http_not_found_handler([](httplib::server::request&, httplib::server::response& resp)
                                           { set_text(resp, "not-found", http::status::not_found); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/regex/abc"));
    REQUIRE(resp.result() == http::status::not_found);
    REQUIRE(as_string(resp) == "not-found");
}

TEST_CASE("Router: wildcard path *", "[router]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/files/*",
                                                  [](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      auto rest = req.path_param("*");
                                                      REQUIRE(rest == "sub/deep/file.txt");
                                                      set_text(resp, rest);
                                                  });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/files/sub/deep/file.txt"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "sub/deep/file.txt");
}

TEST_CASE("Router: multiple HTTP verbs on one route", "[router]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get, http::verb::post>(
        "/multi-verb",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            if (req.method() == http::verb::get)
            {
                set_text(resp, "get-response");
            }
            else
            {
                set_text(resp, "post-response");
            }
        });
    ts.start();

    auto resp_get = UNWRAP(ts.client->get("/multi-verb"));
    REQUIRE(resp_get.result() == http::status::ok);
    REQUIRE(as_string(resp_get) == "get-response");

    auto resp_post = UNWRAP(ts.client->post("/multi-verb", ""sv));
    REQUIRE(resp_post.result() == http::status::ok);
    REQUIRE(as_string(resp_post) == "post-response");
}

TEST_CASE("Router: multiple named parameters", "[router]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/blog/:year/:month/:slug",
                                                  [](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      auto y = req.path_param("year");
                                                      auto m = req.path_param("month");
                                                      auto s = req.path_param("slug");
                                                      REQUIRE(y == "2024");
                                                      REQUIRE(m == "12");
                                                      REQUIRE(s == "hello-world");
                                                      set_text(resp, "ok");
                                                  });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/blog/2024/12/hello-world"));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("Router: set_post_routing_handler for CORS", "[router]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::options>("/*",
                                                      [](httplib::server::request&, httplib::server::response& resp)
                                                      {
                                                          resp.set("Access-Control-Allow-Origin", "*");
                                                          resp.set("Access-Control-Allow-Methods", "GET, POST");
                                                          resp.set_empty_content(http::status::no_content);
                                                      });
    ts.router().set_post_routing_handler(
        [](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set("Access-Control-Allow-Origin", "*");
            resp.set("Access-Control-Allow-Methods", "GET, POST");
        });
    ts.router().set_http_handler<http::verb::post>("/api/data",
                                                   [](httplib::server::request&, httplib::server::response& resp)
                                                   { set_text(resp, "data-ok"); });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/api/data", "{}"sv));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
    REQUIRE(as_string(resp) == "data-ok");
}

struct test_handler
{
    std::string prefix;

    void
    handle(httplib::server::request& req, httplib::server::response& resp)
    {
        set_text(resp, prefix + "-" + std::string(req.path_param("name")));
    }
};

TEST_CASE("Router: member function handler", "[router]")
{
    test_scaffold ts;
    test_handler th { "member" };
    ts.router().set_http_handler<http::verb::get>("/member/:name", &test_handler::handle, th);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/member/test"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "member-test");
}

TEST_CASE("Router: 405 Method Not Allowed", "[router]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/readonly",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { set_text(resp, "get-only"); });
    ts.start();

    auto resp = UNWRAP(ts.client->post("/readonly", ""sv));
    REQUIRE(resp.result() == http::status::method_not_allowed);
    REQUIRE(resp["Allow"] == "GET");
}

TEST_CASE("Router: static path priority over param", "[router]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/users/all",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { set_text(resp, "all-users"); });
    ts.router().set_http_handler<http::verb::get>("/users/:id",
                                                  [](httplib::server::request& req, httplib::server::response& resp)
                                                  { set_text(resp, "user-" + std::string(req.path_param("id"))); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/users/all"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "all-users");

    auto resp2 = UNWRAP(ts.client->get("/users/42"));
    REQUIRE(resp2.result() == http::status::ok);
    REQUIRE(as_string(resp2) == "user-42");
}

TEST_CASE("Router: regex param error in pre_routing", "[router]")
{
    test_scaffold ts;
    REQUIRE_THROWS_AS(
        ts.router().set_http_handler<http::verb::get>("/bad/{id:[}",
                                                      [](httplib::server::request&, httplib::server::response&) {}),
        std::regex_error);
}

TEST_CASE("Router: trailing slash exact match", "[router]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/page/",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { set_text(resp, "slash"); });
    ts.router().set_http_handler<http::verb::get>("/page",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { set_text(resp, "no-slash"); });
    ts.start();

    auto r1 = UNWRAP(ts.client->get("/page/"));
    REQUIRE(as_string(r1) == "slash");

    auto r2 = UNWRAP(ts.client->get("/page"));
    REQUIRE(as_string(r2) == "no-slash");
}

TEST_CASE("Router: returns 404 for missing route", "[router]")
{
    test_scaffold ts;
    ts.start();

    auto resp = UNWRAP(ts.client->get("/nonexistent"));
    REQUIRE(resp.result() == http::status::not_found);
}
