#include "common.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <string>

using namespace httplib;

namespace
{

    struct test_scaffold
    {
        net::io_context ioc;
        server::http_server upstream_server;
        server::http_server proxy_server;

        std::unique_ptr<client::http_client> upstream_client;
        std::unique_ptr<client::http_client> proxy_client;

        std::string upstream_host;
        uint16_t upstream_port = 0;
        uint16_t proxy_port = 0;

        std::thread worker;

        test_scaffold() : upstream_server(ioc), proxy_server(ioc) {}

        void
        start()
        {
            upstream_server.run();
            proxy_server.run();
            worker = std::thread([this] { ioc.run(); });

            upstream_client = std::make_unique<client::http_client>(ioc.get_executor(), upstream_host, upstream_port);
            upstream_client->set_timeout(std::chrono::seconds(5));

            proxy_client = std::make_unique<client::http_client>(ioc.get_executor(), "127.0.0.1", proxy_port);
            proxy_client->set_timeout(std::chrono::seconds(10));
        }

        ~test_scaffold()
        {
            upstream_server.stop().wait();
            proxy_server.stop().wait();
            ioc.stop();
            if (worker.joinable())
            {
                worker.join();
            }
        }
    };

    std::string
    as_string(auto& msg)
    {
        if (msg.body().is_body_type<body::empty_body>())
        {
            return {};
        }
        return msg.body().as<body::string_body>();
    }

} // namespace

TEST_CASE("reverse-proxy", "[proxy]")
{
    test_scaffold ts;

    ts.upstream_server.listen("127.0.0.1", 0);
    ts.upstream_host = ts.upstream_server.local_endpoint().address().to_string();
    ts.upstream_port = ts.upstream_server.local_endpoint().port();

    ts.proxy_server.listen("127.0.0.1", 0);
    ts.proxy_port = ts.proxy_server.local_endpoint().port();

    ts.upstream_server.router()
        .set_http_handler<http::verb::get, http::verb::post, http::verb::put, http::verb::patch, http::verb::delete_>(
            "/echo",
            [](server::request& req, server::response& resp)
            {
                auto body = as_string(req);
                resp.set_string_content(body, "text/plain");
            });

    ts.upstream_server.router().set_http_handler<http::verb::get>(
        "/status/:code",
        [](server::request& req, server::response& resp)
        {
            auto code = std::stoi(std::string(req.path_param("code")));
            resp.set_empty_content(static_cast<http::status>(code));
        });

    ts.upstream_server.router().set_http_handler<http::verb::get>("/headers",
                                                                  [](server::request& req, server::response& resp)
                                                                  {
                                                                      auto xff = req["X-Forwarded-For"];
                                                                      resp.set_string_content(std::string(xff),
                                                                                              "text/plain");
                                                                  });

    ts.upstream_server.router().set_http_handler<http::verb::get>(
        "/resource",
        [](server::request&, server::response& resp)
        { resp.set_string_content(std::string("upstream-resource"), "text/plain"); });

    ts.upstream_server.router().set_http_handler<http::verb::post>(
        "/body-size",
        [](server::request& req, server::response& resp)
        {
            auto body = as_string(req);
            resp.set_string_content(std::to_string(body.size()), "text/plain");
        });

    ts.upstream_server.router().set_http_handler<http::verb::put>("/echo-put",
                                                                  [](server::request& req, server::response& resp)
                                                                  {
                                                                      auto body = as_string(req);
                                                                      resp.set_string_content(body, "text/plain");
                                                                  });

    ts.upstream_server.router().set_http_handler<http::verb::get>(
        "/empty-ok",
        [](server::request&, server::response& resp) { resp.set_string_content(std::string(), "text/plain"); });

    ts.upstream_server.router().set_http_handler<http::verb::get>(
        "/redirect",
        [](server::request&, server::response& resp)
        { resp.set_redirect("/resource", http::status::moved_permanently); });

    ts.upstream_server.router().set_http_handler<http::verb::get>(
        "/check-host",
        [](server::request& req, server::response& resp)
        { resp.set_string_content(std::string(req["Host"]), "text/plain"); });

    ts.proxy_server.set_reverse_proxy("/api/*",
                                      std::string("http://") + ts.upstream_host + ":"
                                          + std::to_string(ts.upstream_port));

    ts.start();

    SECTION("GET /api/resource → upstream /resource")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/resource"));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp) == "upstream-resource");
    }

    SECTION("POST /api/echo with body → upstream /echo")
    {
        // verify upstream handles POST directly
        auto direct = UNWRAP(ts.upstream_client->post("/echo", std::string_view("direct-post")));
        REQUIRE(direct.result() == http::status::ok);
        REQUIRE(as_string(direct) == "direct-post");

        auto resp = UNWRAP(ts.proxy_client->post("/api/echo", std::string_view("hello-proxy")));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp) == "hello-proxy");
    }

    SECTION("GET /api/echo → upstream /echo (empty body)")
    {
        // verify upstream handles GET directly
        auto direct = UNWRAP(ts.upstream_client->get("/echo"));
        REQUIRE(direct.result() == http::status::ok);
        REQUIRE(as_string(direct).empty());

        auto resp = UNWRAP(ts.proxy_client->get("/api/echo"));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp).empty());
    }

    SECTION("GET /api/status/201 → upstream /status/201")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/201"));
        REQUIRE(resp.result() == http::status::created);
    }

    SECTION("GET /api/status/404 → upstream /status/404")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/404"));
        REQUIRE(resp.result() == http::status::not_found);
    }

    SECTION("GET /api/status/204 → upstream /status/204")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/204"));
        REQUIRE(resp.result() == http::status::no_content);
    }

    SECTION("GET /api/status/304 → upstream /status/304")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/304"));
        REQUIRE(resp.result() == http::status::not_modified);
    }

    SECTION("GET /api/status/102 → upstream /status/102 (1xx)")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/102"));
        REQUIRE(resp.result() == http::status::processing);
    }

    SECTION("X-Forwarded-For is appended")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/headers"));
        REQUIRE(resp.result() == http::status::ok);
        auto body = as_string(resp);
        REQUIRE(body.find("127.0.0.1") != std::string::npos);
    }

    SECTION("Host header is set to upstream host")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/check-host"));
        REQUIRE(resp.result() == http::status::ok);
        auto body = as_string(resp);
        REQUIRE(body == ts.upstream_host + ":" + std::to_string(ts.upstream_port));
    }

    SECTION("path not matching prefix returns 404")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/other/resource"));
        REQUIRE(resp.result() == http::status::not_found);
    }

    SECTION("POST with large body")
    {
        std::string large_body(10000, 'x');
        auto direct = UNWRAP(ts.upstream_client->post("/body-size", large_body));
        REQUIRE(direct.result() == http::status::ok);
        REQUIRE(as_string(direct) == "10000");

        auto resp = UNWRAP(ts.proxy_client->post("/api/body-size", large_body));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp) == "10000");
    }

    SECTION("PUT with body")
    {
        auto direct = UNWRAP(ts.upstream_client->put("/echo-put", std::string_view("put-data")));
        REQUIRE(direct.result() == http::status::ok);
        REQUIRE(as_string(direct) == "put-data");

        auto resp = UNWRAP(ts.proxy_client->put("/api/echo-put", std::string_view("put-data")));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp) == "put-data");
    }

    SECTION("POST with empty body")
    {
        auto direct = UNWRAP(ts.upstream_client->post("/echo", std::string_view("")));
        REQUIRE(direct.result() == http::status::ok);
        REQUIRE(as_string(direct).empty());

        auto resp = UNWRAP(ts.proxy_client->post("/api/echo", std::string_view("")));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp).empty());
    }

    SECTION("repeated requests")
    {
        for (int i = 0; i < 5; ++i)
        {
            auto resp1 = UNWRAP(ts.proxy_client->get("/api/resource"));
            REQUIRE(resp1.result() == http::status::ok);
            REQUIRE(as_string(resp1) == "upstream-resource");

            auto resp2 = UNWRAP(
                ts.proxy_client->post("/api/echo", std::string_view("p" + std::to_string(i)), html::query_params {}));
            REQUIRE(resp2.result() == http::status::ok);
            REQUIRE(as_string(resp2) == "p" + std::to_string(i));
        }
    }

    SECTION("status 200 then 404 on same pool connection")
    {
        for (int i = 0; i < 5; ++i)
        {
            auto r = UNWRAP(ts.proxy_client->get("/api/status/200"));
            REQUIRE(r.result() == http::status::ok);
            r = UNWRAP(ts.proxy_client->get("/api/status/404"));
            REQUIRE(r.result() == http::status::not_found);
            r = UNWRAP(ts.proxy_client->get("/api/status/204"));
            REQUIRE(r.result() == http::status::no_content);
        }
    }

    SECTION("mixed requests with body then status")
    {
        for (int i = 0; i < 5; ++i)
        {
            auto r = UNWRAP(
                ts.proxy_client->post("/api/echo", std::string_view("b" + std::to_string(i)), html::query_params {}));
            REQUIRE(r.result() == http::status::ok);
            r = UNWRAP(ts.proxy_client->get("/api/status/404"));
            REQUIRE(r.result() == http::status::not_found);
            r = UNWRAP(ts.proxy_client->get("/api/status/200"));
            REQUIRE(r.result() == http::status::ok);
        }
    }

    SECTION("redirect (301) proxying")
    {
        auto direct = UNWRAP(ts.upstream_client->get("/redirect"));
        REQUIRE(direct.result() == http::status::moved_permanently);

        auto resp = UNWRAP(ts.proxy_client->get("/api/redirect"));
        REQUIRE(resp.result() == http::status::moved_permanently);
    }

    SECTION("empty Content-Length:0 response")
    {
        auto direct = UNWRAP(ts.upstream_client->get("/empty-ok"));
        REQUIRE(direct.result() == http::status::ok);
        REQUIRE(as_string(direct).empty());

        auto resp = UNWRAP(ts.proxy_client->get("/api/empty-ok"));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp).empty());
    }
}
