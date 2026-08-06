#include "common.hpp"
#include "httplib/client/ws_client.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <string>

using namespace httplib;

namespace
{

    struct test_scaffold
    {
        net::thread_pool ioc;
        server::http_server upstream_server;
        server::http_server proxy_server;

        std::unique_ptr<client::http_client> upstream_client;
        std::unique_ptr<client::http_client> proxy_client;

        std::string upstream_host;
        uint16_t upstream_port = 0;
        uint16_t proxy_port = 0;

        test_scaffold() : upstream_server(ioc.get_executor()), proxy_server(ioc.get_executor()) {}

        void
        start()
        {
            upstream_server.run();
            proxy_server.run();

            upstream_client = std::make_unique<client::http_client>(ioc.get_executor(), upstream_host, upstream_port);
            upstream_client->set_timeout(std::chrono::seconds(5));

            proxy_client = std::make_unique<client::http_client>(ioc.get_executor(), "127.0.0.1", proxy_port);
            proxy_client->set_timeout(std::chrono::seconds(10));
        }

        ~test_scaffold()
        {
            upstream_server.stop().wait();
            proxy_server.stop().wait();
            ioc.join();
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

    SECTION("GET /api/resource -> upstream /resource")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/resource"));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp) == "upstream-resource");
    }

    SECTION("POST /api/echo with body -> upstream /echo")
    {
        // verify upstream handles POST directly
        auto direct = UNWRAP(ts.upstream_client->post("/echo", std::string_view("direct-post")));
        REQUIRE(direct.result() == http::status::ok);
        REQUIRE(as_string(direct) == "direct-post");

        auto resp = UNWRAP(ts.proxy_client->post("/api/echo", std::string_view("hello-proxy")));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp) == "hello-proxy");
    }

    SECTION("GET /api/echo -> upstream /echo (empty body)")
    {
        // verify upstream handles GET directly
        auto direct = UNWRAP(ts.upstream_client->get("/echo"));
        REQUIRE(direct.result() == http::status::ok);
        REQUIRE(as_string(direct).empty());

        auto resp = UNWRAP(ts.proxy_client->get("/api/echo"));
        REQUIRE(resp.result() == http::status::ok);
        REQUIRE(as_string(resp).empty());
    }

    SECTION("GET /api/status/201 -> upstream /status/201")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/201"));
        REQUIRE(resp.result() == http::status::created);
    }

    SECTION("GET /api/status/404 -> upstream /status/404")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/404"));
        REQUIRE(resp.result() == http::status::not_found);
    }

    SECTION("GET /api/status/204 -> upstream /status/204")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/204"));
        REQUIRE(resp.result() == http::status::no_content);
    }

    SECTION("GET /api/status/304 -> upstream /status/304")
    {
        auto resp = UNWRAP(ts.proxy_client->get("/api/status/304"));
        REQUIRE(resp.result() == http::status::not_modified);
    }

    SECTION("GET /api/status/102 -> upstream /status/102 (1xx)")
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

TEST_CASE("CookieProxy: via reverse proxy rewrites Cookie header", "[proxy]")
{
    net::thread_pool ioc;
    httplib::server::http_server upstream(ioc.get_executor());
    httplib::server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    auto upstream_host = upstream.local_endpoint().address().to_string();
    auto upstream_port = upstream.local_endpoint().port();

    proxy.listen("127.0.0.1", 0);
    auto proxy_port = proxy.local_endpoint().port();

    upstream.router().set_http_handler<http::verb::get>(
        "/check-cookie",
        [](httplib::server::request& req, httplib::server::response& resp)
        { resp.set_string_content(std::string(req["Cookie"]), "text/plain"); });

    proxy.set_reverse_proxy("/api/*", std::format("http://{}:{}", upstream_host, upstream_port));

    upstream.run();
    proxy.run();

    auto client = std::make_unique<httplib::client::http_client>(ioc.get_executor(), "127.0.0.1", proxy_port);
    client->set_timeout(std::chrono::seconds(5));

    auto req_headers = httplib::http::fields();
    req_headers.set(http::field::cookie, "token=abc; Domain=upstream.com; Path=/api");
    auto resp = UNWRAP(client->send_request(http::verb::get, "/api/check-cookie", req_headers));
    REQUIRE(resp.result() == http::status::ok);
    auto body = as_string(resp);
    REQUIRE(body.find("token=abc") != std::string::npos);
    REQUIRE(body.find("Domain=upstream.com") == std::string::npos);
    REQUIRE(body.find("Path=/api") == std::string::npos);
    REQUIRE(body.find("Domain=" + upstream_host) != std::string::npos);
    REQUIRE(body.find("Path=/") != std::string::npos);

    upstream.stop();
    proxy.stop();
    ioc.join();
}

TEST_CASE("Proxy: rewrites Referer to upstream", "[proxy]")
{
    net::thread_pool ioc;
    httplib::server::http_server upstream(ioc.get_executor());
    httplib::server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    auto upstream_host = upstream.local_endpoint().address().to_string();
    auto upstream_port = upstream.local_endpoint().port();

    proxy.listen("127.0.0.1", 0);
    auto proxy_port = proxy.local_endpoint().port();

    upstream.router().set_http_handler<http::verb::get>(
        "/echo-referer",
        [](httplib::server::request& req, httplib::server::response& resp)
        { resp.set_string_content(std::string(req[http::field::referer]), "text/plain"); });

    proxy.set_reverse_proxy("/api/*", std::format("http://{}:{}", upstream_host, upstream_port));

    upstream.run();
    proxy.run();

    auto client = std::make_unique<httplib::client::http_client>(ioc.get_executor(), "127.0.0.1", proxy_port);
    client->set_timeout(std::chrono::seconds(5));

    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::referer, std::format("http://127.0.0.1:{}/api/some-page?a=1&b=2#sec", proxy_port));
    auto resp = UNWRAP(client->send_request(http::verb::get, "/api/echo-referer", hdrs));
    REQUIRE(resp.result() == http::status::ok);
    auto body = as_string(resp);
    REQUIRE(body.find(upstream_host) != std::string::npos);
    REQUIRE(body.find("/some-page?a=1&b=2#sec") != std::string::npos);
    REQUIRE(body.find("/api") == std::string::npos);

    upstream.stop();
    proxy.stop();
    ioc.join();
}

TEST_CASE("Proxy: forwards X-Forwarded-Proto and X-Forwarded-Host", "[proxy]")
{
    net::thread_pool ioc;
    httplib::server::http_server upstream(ioc.get_executor());
    httplib::server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    auto upstream_host = upstream.local_endpoint().address().to_string();
    auto upstream_port = upstream.local_endpoint().port();

    proxy.listen("127.0.0.1", 0);
    auto proxy_port = proxy.local_endpoint().port();

    upstream.router().set_http_handler<http::verb::get>(
        "/echo-headers",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            auto proto = req["X-Forwarded-Proto"];
            auto host = req["X-Forwarded-Host"];
            resp.set_string_content(std::format("proto={} host={}", std::string(proto), std::string(host)),
                                    "text/plain");
        });

    proxy.set_reverse_proxy("/api/*", std::format("http://{}:{}", upstream_host, upstream_port));

    upstream.run();
    proxy.run();

    auto client = std::make_unique<httplib::client::http_client>(ioc.get_executor(), "127.0.0.1", proxy_port);
    client->set_timeout(std::chrono::seconds(5));

    auto resp = UNWRAP(client->get("/api/echo-headers"));
    REQUIRE(resp.result() == http::status::ok);
    auto body = as_string(resp);
    REQUIRE(body.find("proto=http") != std::string::npos);
    REQUIRE(body.find("host=127.0.0.1:") != std::string::npos);

    upstream.stop();
    proxy.stop();
    ioc.join();
}

TEST_CASE("ws-forward echo", "[proxy][ws-forward]")
{
    net::thread_pool ioc;
    server::http_server upstream(ioc.get_executor());
    server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    proxy.listen("127.0.0.1", 0);
    upstream.run();
    proxy.run();

    auto upstream_ep = upstream.local_endpoint();
    auto proxy_ep = proxy.local_endpoint();
    std::string upstream_url = std::format("ws://{}:{}", upstream_ep.address().to_string(), upstream_ep.port());

    upstream.router().set_ws_handler(
        "/extra-path",
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [](server::websocket_conn::weak_ptr wp, std::string_view msg, bool binary) -> net::awaitable<void>
        {
            if (auto c = wp.lock())
            {
                c->send(msg, binary);
            }
            co_return;
        },
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });

    proxy.set_ws_forward("/ws/*", upstream_url);

    client::ws_client ws(ioc.get_executor(), proxy_ep.address().to_string(), proxy_ep.port());
    std::vector<std::string> received;
    std::string large_data(1024 * 64, 'X');
    for (std::size_t i = 0; i < large_data.size(); ++i)
    {
        large_data[i] = static_cast<char>(i % 256);
    }

    ws.set_handler(
        [&](boost::system::error_code ec) -> net::awaitable<void>
        {
            REQUIRE(!ec);
            ws.send(std::string("hello-forward"));
            ws.send(std::string("message-two"));
            ws.send(std::string(large_data), true);
            ws.send(std::string("final"));
            co_return;
        },
        [&](std::string_view msg, bool binary) -> net::awaitable<void>
        {
            received.emplace_back(msg);
            if (received.size() >= 4)
            {
                ws.close();
            }
            co_return;
        },
        []() -> net::awaitable<void> { co_return; });

    ws.run("/ws/extra-path");

    std::this_thread::sleep_for(std::chrono::seconds(5));

    upstream.stop();
    proxy.stop();
    ioc.join();

    REQUIRE(received.size() == 4);
    REQUIRE(received[0] == "hello-forward");
    REQUIRE(received[1] == "message-two");
    REQUIRE(received[2] == large_data);
    REQUIRE(received[3] == "final");
}

TEST_CASE("ws-forward stress: concurrent connections + shutdown", "[proxy][ws-forward]")
{
    net::thread_pool ioc;
    server::http_server upstream(ioc.get_executor());
    server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    proxy.listen("127.0.0.1", 0);
    upstream.run();
    proxy.run();

    auto upstream_ep = upstream.local_endpoint();
    auto proxy_ep = proxy.local_endpoint();
    std::string upstream_url = std::format("ws://{}:{}", upstream_ep.address().to_string(), upstream_ep.port());

    upstream.router().set_ws_handler(
        "/echo",
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [](server::websocket_conn::weak_ptr wp, std::string_view msg, bool binary) -> net::awaitable<void>
        {
            if (auto c = wp.lock())
            {
                c->send(msg, binary);
            }
            co_return;
        },
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });

    proxy.set_ws_forward("/ws/*", upstream_url);

    constexpr int kConnections = 6;
    std::atomic<int> connected { 0 };
    std::atomic<int> total_sent { 0 };
    std::atomic<int> total_recv { 0 };
    std::atomic<int> closed { 0 };
    std::atomic<bool> stop_flag { false };
    std::vector<std::unique_ptr<client::ws_client>> clients;

    upstream.router().set_ws_handler(
        "/echo",
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [&](server::websocket_conn::weak_ptr wp, std::string_view msg, bool binary) -> net::awaitable<void>
        {
            ++total_recv;
            if (auto c = wp.lock())
            {
                c->send(msg, binary);
            }
            co_return;
        },
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });

    proxy.set_ws_forward("/ws/*", upstream_url);

    for (int i = 0; i < kConnections; ++i)
    {
        auto ws
            = std::make_unique<client::ws_client>(ioc.get_executor(), proxy_ep.address().to_string(), proxy_ep.port());
        ws->set_handler(
            [&, ws = ws.get()](boost::system::error_code ec) -> net::awaitable<void>
            {
                if (!ec)
                {
                    ++connected;
                    for (int j = 0; j < 2000 && !stop_flag.load(); ++j)
                    {
                        ws->send(std::format("{}", j));
                        ++total_sent;
                    }
                }
                co_return;
            },
            [](std::string_view, bool) -> net::awaitable<void> { co_return; },
            [&]() -> net::awaitable<void>
            {
                ++closed;
                co_return;
            });
        clients.push_back(std::move(ws));
    }

    for (auto& c : clients)
    {
        c->run("/ws/echo");
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));

    stop_flag.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    upstream.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    proxy.stop();
    ioc.join();

    REQUIRE(total_sent.load() > 0);
    REQUIRE(total_recv.load() > 0);
}

TEST_CASE("proxy-rewrite-redirect-location", "[proxy]")
{
    net::thread_pool ioc;
    server::http_server upstream(ioc.get_executor());
    server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    proxy.listen("127.0.0.1", 0);
    upstream.run();
    proxy.run();

    auto upstream_ep = upstream.local_endpoint();
    auto upstream_host = upstream_ep.address().to_string();
    auto upstream_port = upstream_ep.port();

    upstream.router().set_http_handler<http::verb::get>(
        "/redirect-me",
        [&](server::request&, server::response& resp)
        {
            resp.set_redirect(std::format("http://{}:{}/new-place", upstream_host, upstream_port),
                              http::status::moved_permanently);
        });

    upstream.router().set_http_handler<http::verb::get>(
        "/redirect-root",
        [&](server::request&, server::response& resp)
        { resp.set_redirect(std::format("http://{}:{}/", upstream_host, upstream_port), http::status::found); });

    proxy.set_reverse_proxy("/api/*", std::format("http://{}:{}", upstream_host, upstream_port));

    auto client = client::http_client(ioc.get_executor(), "127.0.0.1", proxy.local_endpoint().port());
    client.set_timeout(std::chrono::seconds(5));
    client.set_max_redirects(0);

    auto resp = UNWRAP(client.get("/api/redirect-me"));
    REQUIRE(resp.result() == http::status::moved_permanently);
    REQUIRE(std::string(resp["Location"]) == "/api/new-place");

    auto resp2 = UNWRAP(client.get("/api/redirect-root"));
    REQUIRE(resp2.result() == http::status::found);
    REQUIRE(std::string(resp2["Location"]) == "/api/");

    upstream.stop();
    proxy.stop();
    ioc.join();
}

TEST_CASE("proxy-rewrite-redirect-with-base-path", "[proxy]")
{
    net::thread_pool ioc;
    server::http_server upstream(ioc.get_executor());
    server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    proxy.listen("127.0.0.1", 0);
    upstream.run();
    proxy.run();

    auto upstream_ep = upstream.local_endpoint();
    auto upstream_host = upstream_ep.address().to_string();
    auto upstream_port = upstream_ep.port();

    upstream.router().set_http_handler<http::verb::get>(
        "/qqq/redirect-me",
        [&](server::request&, server::response& resp)
        {
            resp.set_redirect(std::format("http://{}:{}/new-place", upstream_host, upstream_port),
                              http::status::moved_permanently);
        });

    proxy.set_reverse_proxy("/api/*", std::format("http://{}:{}/qqq", upstream_host, upstream_port));

    auto client = client::http_client(ioc.get_executor(), "127.0.0.1", proxy.local_endpoint().port());
    client.set_timeout(std::chrono::seconds(5));
    client.set_max_redirects(0);

    auto resp = UNWRAP(client.get("/api/redirect-me"));
    REQUIRE(resp.result() == http::status::moved_permanently);
    REQUIRE(std::string(resp["Location"]) == "/api/new-place");

    upstream.stop();
    proxy.stop();
    ioc.join();
}

TEST_CASE("proxy-interceptor: all steps called", "[proxy]")
{
    net::thread_pool ioc;
    server::http_server upstream(ioc.get_executor());
    server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    proxy.listen("127.0.0.1", 0);
    upstream.run();
    proxy.run();

    auto upstream_ep = upstream.local_endpoint();
    auto proxy_ep = proxy.local_endpoint();
    std::string upstream_url = std::format("http://{}:{}", upstream_ep.address().to_string(), upstream_ep.port());

    upstream.router().set_http_handler<http::verb::post>(
        "/echo",
        [](server::request& req, server::response& resp)
        { resp.set_string_content(std::string(req.body().as<body::string_body>()), "text/plain"); });

    std::atomic<int> req_called { 0 }, req_body_called { 0 }, resp_called { 0 }, resp_body_called { 0 };

    struct test_interceptor : server::proxy_interceptor
    {
        std::atomic<int>* req_called;
        std::atomic<int>* req_body_called;
        std::atomic<int>* resp_called;
        std::atomic<int>* resp_body_called;

        net::awaitable<void>
        on_upstream_request(server::request&, http::fields&, std::string const&) override
        {
            (*req_called)++;
            co_return;
        }
        net::awaitable<void>
        on_upstream_request_body(net::const_buffer, bool) override
        {
            (*req_body_called)++;
            co_return;
        }
        net::awaitable<void>
        on_upstream_response(server::request&, http::status, http::fields const&) override
        {
            (*resp_called)++;
            co_return;
        }
        net::awaitable<void>
        on_upstream_response_body(net::const_buffer, bool) override
        {
            (*resp_body_called)++;
            co_return;
        }
    };

    proxy.set_reverse_proxy("/api/*",
                            upstream_url,
                            [&](server::request&) -> std::shared_ptr<server::proxy_interceptor>
                            {
                                auto ti = std::make_shared<test_interceptor>();
                                ti->req_called = &req_called;
                                ti->req_body_called = &req_body_called;
                                ti->resp_called = &resp_called;
                                ti->resp_body_called = &resp_body_called;
                                return ti;
                            });

    auto client = client::http_client(ioc.get_executor(), proxy_ep.address().to_string(), proxy_ep.port());
    client.set_timeout(std::chrono::seconds(5));
    auto resp = UNWRAP(client.post("/api/echo", std::string_view("hello")));

    upstream.stop();
    proxy.stop();
    ioc.join();

    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "hello");
    REQUIRE(req_called.load() == 1);
    REQUIRE(req_body_called.load() >= 1);
    REQUIRE(resp_called.load() == 1);
    REQUIRE(resp_body_called.load() >= 1);
}

TEST_CASE("ws-interceptor: messages intercepted", "[proxy][ws-forward]")
{
    net::thread_pool ioc;
    server::http_server upstream(ioc.get_executor());
    server::http_server proxy(ioc.get_executor());

    upstream.listen("127.0.0.1", 0);
    proxy.listen("127.0.0.1", 0);
    upstream.run();
    proxy.run();

    auto upstream_ep = upstream.local_endpoint();
    auto proxy_ep = proxy.local_endpoint();
    std::string upstream_url = std::format("ws://{}:{}", upstream_ep.address().to_string(), upstream_ep.port());

    upstream.router().set_ws_handler(
        "/echo",
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [](server::websocket_conn::weak_ptr wp, std::string_view msg, bool binary) -> net::awaitable<void>
        {
            if (auto c = wp.lock())
            {
                c->send(msg, binary);
            }
            co_return;
        },
        [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });

    std::atomic<int> req_called { 0 }, client_msg_called { 0 }, upstream_msg_called { 0 };

    struct test_ws_interceptor : server::ws_interceptor
    {
        std::atomic<int>* req_called;
        std::atomic<int>* client_msg_called;
        std::atomic<int>* upstream_msg_called;

        net::awaitable<void>
        on_upstream_request(server::request&, http::fields&, std::string const&) override
        {
            (*req_called)++;
            co_return;
        }
        net::awaitable<void>
        on_upstream_send(std::string_view, bool) override
        {
            (*client_msg_called)++;
            co_return;
        }
        net::awaitable<void>
        on_upstream_recv(std::string_view, bool) override
        {
            (*upstream_msg_called)++;
            co_return;
        }
    };

    proxy.set_ws_forward("/ws/*",
                         upstream_url,
                         [&](server::request&) -> std::shared_ptr<server::ws_interceptor>
                         {
                             auto ti = std::make_shared<test_ws_interceptor>();
                             ti->req_called = &req_called;
                             ti->client_msg_called = &client_msg_called;
                             ti->upstream_msg_called = &upstream_msg_called;
                             return ti;
                         });

    client::ws_client ws(ioc.get_executor(), proxy_ep.address().to_string(), proxy_ep.port());
    std::vector<std::string> received;
    ws.set_handler(
        [&](boost::system::error_code ec) -> net::awaitable<void>
        {
            REQUIRE(!ec);
            ws.send(std::string("hello"));
            ws.send(std::string("world"));
            ws.send(std::string("done"));
            co_return;
        },
        [&](std::string_view msg, bool) -> net::awaitable<void>
        {
            received.emplace_back(msg);
            if (received.size() >= 3)
            {
                ws.close();
            }
            co_return;
        },
        []() -> net::awaitable<void> { co_return; });

    ws.run("/ws/echo");

    std::this_thread::sleep_for(std::chrono::seconds(5));
    upstream.stop();
    proxy.stop();
    ioc.join();

    REQUIRE(received.size() == 3);
    REQUIRE(received[0] == "hello");
    REQUIRE(received[1] == "world");
    REQUIRE(received[2] == "done");
    REQUIRE(req_called.load() == 1);
    REQUIRE(client_msg_called.load() == 3);
    REQUIRE(upstream_msg_called.load() == 3);
}
