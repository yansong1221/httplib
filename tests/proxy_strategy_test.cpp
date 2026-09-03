#include "common.hpp"
#include "httplib/client/response.hpp"
#include "httplib/server/proxy_strategy.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "server/request_impl.hpp"
#include "server/upstream_group.hpp"
#include <catch2/catch_test_macros.hpp>
#include <map>

using namespace httplib;
using namespace httplib::server;

namespace
{
    std::string
    as_string(httplib::client::response& resp)
    {
        return resp.as_string();
    }

    std::vector<upstream_backend>
    make_weighted(std::initializer_list<std::pair<std::string_view, uint32_t>> items)
    {
        std::vector<upstream_backend> result;
        result.reserve(items.size());
        for (auto [url, weight] : items)
        {
            result.push_back(upstream_backend { std::string(url), weight });
        }
        return result;
    }
} // namespace

TEST_CASE("upstream_group: make_backends from config")
{
    auto group = std::make_shared<upstream_group>(make_backends({
        upstream_backend { "http://a:80" },
        upstream_backend { "http://b:80", 3 }
    }));
    REQUIRE(group->size() == 2);
    CHECK(group->at(0).url == "http://a:80");
    CHECK(group->at(0).weight == 1);
    CHECK(group->at(1).weight == 3);
}

TEST_CASE("upstream_group: healthy_backends filters unhealthy")
{
    auto group = std::make_shared<upstream_group>(make_backends({ upstream_backend { "http://a:80" },
                                                                  upstream_backend { "http://b:80" },
                                                                  upstream_backend { "http://c:80" } }));
    group->mark_unhealthy(1);

    auto healthy = group->healthy_backends();
    REQUIRE(healthy.size() == 2);
    CHECK(healthy[0]->url == "http://a:80");
    CHECK(healthy[1]->url == "http://c:80");
}

TEST_CASE("upstream_group: mark_healthy restores backend")
{
    auto group = std::make_shared<upstream_group>(
        make_backends({ upstream_backend { "http://a:80" }, upstream_backend { "http://b:80" } }));
    group->mark_unhealthy(0);
    CHECK(group->healthy_backends().size() == 1);

    group->mark_healthy(0);
    CHECK(group->healthy_backends().size() == 2);
}

// ===========================================================================
// Round-robin (default locator)
// ===========================================================================

TEST_CASE("upstream_group: round_robin cycles through all backends")
{
    auto group = std::make_shared<upstream_group>(make_backends({ upstream_backend { "http://a:80" },
                                                                  upstream_backend { "http://b:80" },
                                                                  upstream_backend { "http://c:80" } }));

    CHECK(group->resolve(upstream_locator::round_robin) == "http://a:80");
    CHECK(group->resolve(upstream_locator::round_robin) == "http://b:80");
    CHECK(group->resolve(upstream_locator::round_robin) == "http://c:80");
    CHECK(group->resolve(upstream_locator::round_robin) == "http://a:80");
    CHECK(group->resolve(upstream_locator::round_robin) == "http://b:80");
    CHECK(group->resolve(upstream_locator::round_robin) == "http://c:80");
}

TEST_CASE("upstream_group: round_robin skips unhealthy backends")
{
    auto group = std::make_shared<upstream_group>(make_backends({ upstream_backend { "http://a:80" },
                                                                  upstream_backend { "http://b:80" },
                                                                  upstream_backend { "http://c:80" } }));
    group->mark_unhealthy(1); // b is down

    CHECK(group->resolve(upstream_locator::round_robin) == "http://a:80");
    CHECK(group->resolve(upstream_locator::round_robin) == "http://c:80");
    CHECK(group->resolve(upstream_locator::round_robin) == "http://a:80");
    CHECK(group->resolve(upstream_locator::round_robin) == "http://c:80");
}

TEST_CASE("upstream_group: round_robin single backend")
{
    auto group = std::make_shared<upstream_group>(make_backends({ upstream_backend { "http://only:80" } }));
    for (int i = 0; i < 5; ++i)
    {
        CHECK(group->resolve(upstream_locator::round_robin) == "http://only:80");
    }
}

TEST_CASE("upstream_group: resolve defaults to round_robin")
{
    auto group = std::make_shared<upstream_group>(
        make_backends({ upstream_backend { "http://a:80" }, upstream_backend { "http://b:80" } }));
    CHECK(group->resolve() == "http://a:80");
    CHECK(group->resolve() == "http://b:80");
}

// ===========================================================================
// Weighted round-robin
// ===========================================================================

TEST_CASE("upstream_group: weighted_round_robin respects weights")
{
    auto group = std::make_shared<upstream_group>(make_backends(make_weighted({
        { "http://a:80", 3 },
        { "http://b:80", 1 }
    })));

    std::map<std::string, size_t> counts;
    for (int i = 0; i < 12; ++i)
    {
        counts[group->resolve(upstream_locator::weighted_round_robin)]++;
    }

    CHECK(counts["http://a:80"] >= 7);
    CHECK(counts["http://b:80"] >= 2);
    CHECK(counts["http://a:80"] + counts["http://b:80"] == 12);
}

TEST_CASE("upstream_group: weighted_round_robin skips unhealthy")
{
    auto group = std::make_shared<upstream_group>(make_backends(make_weighted({
        { "http://a:80", 3 },
        { "http://b:80", 1 }
    })));
    group->mark_unhealthy(0); // a is down

    for (int i = 0; i < 4; ++i)
    {
        CHECK(group->resolve(upstream_locator::weighted_round_robin) == "http://b:80");
    }
}

TEST_CASE("upstream_group: weighted_round_robin single backend")
{
    auto group = std::make_shared<upstream_group>(make_backends(make_weighted({
        { "http://only:80", 5 }
    })));
    for (int i = 0; i < 5; ++i)
    {
        CHECK(group->resolve(upstream_locator::weighted_round_robin) == "http://only:80");
    }
}

// ===========================================================================
// Least connections
// ===========================================================================

TEST_CASE("upstream_group: least_connections picks backend with fewest active")
{
    auto group = std::make_shared<upstream_group>(make_backends({ upstream_backend { "http://a:80" },
                                                                  upstream_backend { "http://b:80" },
                                                                  upstream_backend { "http://c:80" } }));

    group->at(0).active.store(5);
    group->at(1).active.store(2);
    group->at(2).active.store(8);

    for (int i = 0; i < 3; ++i)
    {
        CHECK(group->resolve(upstream_locator::least_connections) == "http://b:80");
    }
}

TEST_CASE("upstream_group: least_connections skips unhealthy backends")
{
    auto group = std::make_shared<upstream_group>(
        make_backends({ upstream_backend { "http://a:80" }, upstream_backend { "http://b:80" } }));
    group->at(0).active.store(0);
    group->at(1).active.store(10);
    group->mark_unhealthy(0);

    for (int i = 0; i < 3; ++i)
    {
        CHECK(group->resolve(upstream_locator::least_connections) == "http://b:80");
    }
}

TEST_CASE("upstream_group: least_connections single backend")
{
    auto group = std::make_shared<upstream_group>(make_backends({ upstream_backend { "http://only:80" } }));
    group->at(0).active.store(42);

    for (int i = 0; i < 3; ++i)
    {
        CHECK(group->resolve(upstream_locator::least_connections) == "http://only:80");
    }
}

TEST_CASE("upstream_group: least_connections ties go to first found")
{
    auto group = std::make_shared<upstream_group>(
        make_backends({ upstream_backend { "http://a:80" }, upstream_backend { "http://b:80" } }));
    group->at(0).active.store(3);
    group->at(1).active.store(3);

    for (int i = 0; i < 5; ++i)
    {
        CHECK(group->resolve(upstream_locator::least_connections) == "http://a:80");
    }
}

// ===========================================================================
// Error cases
// ===========================================================================

TEST_CASE("upstream_group: throws when all backends unhealthy")
{
    auto group = std::make_shared<upstream_group>(
        make_backends({ upstream_backend { "http://a:80" }, upstream_backend { "http://b:80" } }));
    group->mark_unhealthy(0);
    group->mark_unhealthy(1);

    CHECK_THROWS(group->resolve(upstream_locator::round_robin));
}

// ===========================================================================
// End-to-end: set_reverse_proxy with upstream backends
// ===========================================================================

TEST_CASE("proxy[group]: distributes round-robin across upstreams", "[proxy][group]")
{
    net::thread_pool pool { 4 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            server::http_server upstream1(pool.get_executor());
            upstream1.router().template set_http_handler<http::verb::get>(
                "/who",
                [](server::request&, server::response& resp)
                { resp.set_string_content(std::string("u1"), "text/plain"); });

            server::http_server upstream2(pool.get_executor());
            upstream2.router().template set_http_handler<http::verb::get>(
                "/who",
                [](server::request&, server::response& resp)
                { resp.set_string_content(std::string("u2"), "text/plain"); });

            auto e1 = upstream1.listen("127.0.0.1", 0).local_endpoint();
            auto e2 = upstream2.listen("127.0.0.1", 0).local_endpoint();

            std::vector<upstream_backend> backends {
                upstream_backend { std::format("http://{}:{}", e1.address().to_string(), e1.port()) },
                upstream_backend { std::format("http://{}:{}", e2.address().to_string(), e2.port()) },
            };

            server::http_server proxy(pool.get_executor());
            proxy.set_reverse_proxy("/api/*", backends, upstream_locator::round_robin);
            proxy.listen("127.0.0.1", 0);
            auto pep = proxy.local_endpoint();

            upstream1.run();
            upstream2.run();
            proxy.run();

            client::http_client c(pool.get_executor(), "127.0.0.1", pep.port());
            c.set_timeout(std::chrono::seconds(10));

            std::vector<std::string> hits;
            for (int i = 0; i < 6; ++i)
            {
                auto resp = UNWRAP(co_await c.async_get("/api/who"));
                REQUIRE(resp.result() == http::status::ok);
                hits.push_back(as_string(resp));
            }

            CHECK(hits[0] == "u1");
            CHECK(hits[1] == "u2");
            CHECK(hits[2] == "u1");
            CHECK(hits[3] == "u2");
            CHECK(hits[4] == "u1");
            CHECK(hits[5] == "u2");

            c.close();
            proxy.stop();
            upstream1.stop();
            upstream2.stop();
        },
        [&](std::exception_ptr e) { err = e; });
    pool.join();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("proxy[group]: least_connections picks least busy upstream", "[proxy][group]")
{
    net::thread_pool pool { 4 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            server::http_server upstream1(pool.get_executor());
            upstream1.router().template set_http_handler<http::verb::get>(
                "/who",
                [](server::request&, server::response& resp)
                { resp.set_string_content(std::string("u1"), "text/plain"); });

            server::http_server upstream2(pool.get_executor());
            upstream2.router().template set_http_handler<http::verb::get>(
                "/who",
                [](server::request&, server::response& resp)
                { resp.set_string_content(std::string("u2"), "text/plain"); });

            auto e1 = upstream1.listen("127.0.0.1", 0).local_endpoint();
            auto e2 = upstream2.listen("127.0.0.1", 0).local_endpoint();

            std::vector<upstream_backend> backends {
                upstream_backend { std::format("http://{}:{}", e1.address().to_string(), e1.port()) },
                upstream_backend { std::format("http://{}:{}", e2.address().to_string(), e2.port()) },
            };

            server::http_server proxy(pool.get_executor());
            proxy.set_reverse_proxy("/api/*", backends, upstream_locator::least_connections);
            proxy.listen("127.0.0.1", 0);
            auto pep = proxy.local_endpoint();

            upstream1.run();
            upstream2.run();
            proxy.run();

            client::http_client c(pool.get_executor(), "127.0.0.1", pep.port());
            c.set_timeout(std::chrono::seconds(10));

            // All connections idle -> least-conn ties to first (u1)
            auto r1 = UNWRAP(co_await c.async_get("/api/who"));
            REQUIRE(as_string(r1) == "u1");

            c.close();
            proxy.stop();
            upstream1.stop();
            upstream2.stop();
        },
        [&](std::exception_ptr e) { err = e; });
    pool.join();
    if (err)
    {
        std::rethrow_exception(err);
    }
}
