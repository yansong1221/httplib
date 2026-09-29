#include "common.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/server.hpp"
#include "server/trusted_proxies.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

using test_common::run;

namespace
{

    /// 起一个把 get_client_ip() 原样回显的服务器。
    ///
    /// 注意：所有测试请求都来自 127.0.0.1，因此**直连对端永远是 127.0.0.1**。
    /// 要验证 X-Forwarded-For 是否被采信，只能靠把这个地址配成可信代理。
    void
    echo_client_ip(httplib::server::http_server& server)
    {
        server.router().set_http_handler(httplib::method::get,
                                         "/ip",
                                         [](httplib::server::request& req, httplib::server::response& resp)
                                         { resp.set_string_content(req.get_client_ip().to_string(), "text/plain"sv); });
    }

    /// 带 X-Forwarded-For 请求 /ip，断言服务端识别出的客户端 IP。
    net::awaitable<void>
    expect_client_ip(httplib::client::http_client& client, std::string_view xff, std::string_view expected)
    {
        auto hdrs = httplib::headers();
        hdrs.set("X-Forwarded-For", std::string(xff));
        httplib::client::request req(httplib::method::get, "/ip", hdrs);
        auto resp = UNWRAP(co_await client.async_send_request(req));
        REQUIRE(resp.result() == httplib::status::ok);
        REQUIRE(resp.as_string() == expected);
        co_return;
    }

    /// 不带 XFF 的请求，对端地址即客户端 IP。
    net::awaitable<void>
    expect_no_xff(httplib::client::http_client& client, std::string_view expected)
    {
        auto resp = UNWRAP(co_await client.async_get("/ip"));
        REQUIRE(resp.result() == httplib::status::ok);
        REQUIRE(resp.as_string() == expected);
        co_return;
    }

} // namespace

TEST_CASE("Client IP: X-Forwarded-For is ignored when no proxy is trusted", "[server]")
{
    // 默认配置：XFF 完全由客户端填写，采信它等于允许任何人自己声明 IP。
    run(echo_client_ip,
        [](auto& client) -> net::awaitable<void>
        {
            co_await expect_client_ip(client, "203.0.113.7", "127.0.0.1");
            co_await expect_client_ip(client, "1.1.1.1, 2.2.2.2", "127.0.0.1");
        });
}

TEST_CASE("Client IP: X-Forwarded-For is honored from a trusted proxy", "[server]")
{
    // 测试客户端直连 127.0.0.1，所以把它配成可信代理。
    run(
        [](auto& server)
        {
            server.set_trusted_proxies({ "127.0.0.1" });
            echo_client_ip(server);
        },
        [](auto& client) -> net::awaitable<void> { co_await expect_client_ip(client, "203.0.113.7", "203.0.113.7"); });
}

TEST_CASE("Client IP: a single trusted address matches exactly, not as a supernet", "[server]")
{
    run(
        [](auto& server)
        {
            server.set_trusted_proxies({ "192.0.2.7" });
            echo_client_ip(server);
        },
        [](auto& client) -> net::awaitable<void>
        {
            // 127.0.0.1 不等于 192.0.2.7 → 对端不可信 → 不采信 XFF。
            co_await expect_client_ip(client, "203.0.113.7", "127.0.0.1");
        });
}

TEST_CASE("Client IP: CIDR matching honours the prefix length", "[server]")
{
    // 127.0.0.0/8 覆盖直连对端 → 采信。
    run(
        [](auto& server)
        {
            server.set_trusted_proxies({ "127.0.0.0/8" });
            echo_client_ip(server);
        },
        [](auto& client) -> net::awaitable<void> { co_await expect_client_ip(client, "203.0.113.7", "203.0.113.7"); });

    // 192.0.2.0/24 不覆盖 127.0.0.1 → 不采信。
    run(
        [](auto& server)
        {
            server.set_trusted_proxies({ "192.0.2.0/24" });
            echo_client_ip(server);
        },
        [](auto& client) -> net::awaitable<void> { co_await expect_client_ip(client, "203.0.113.7", "127.0.0.1"); });
}

TEST_CASE("Client IP: the closest untrusted hop is taken, scanning from the right", "[server]")
{
    // 客户端伪造左侧前缀，只有从右往左跳过可信代理才不会被骗。
    run(
        [](auto& server)
        {
            server.set_trusted_proxies({ "127.0.0.1", "10.0.0.0/8" });
            echo_client_ip(server);
        },
        [](auto& client) -> net::awaitable<void>
        {
            // 右端的 10.0.0.5 是可信代理 → 跳过，取它左边的 198.51.100.9。
            co_await expect_client_ip(client, "198.51.100.9, 10.0.0.5", "198.51.100.9");
        });
}

TEST_CASE("Client IP: a spoofed left-most entry is ignored", "[server]")
{
    // 最常见的错误写法是直接取最左端，客户端预置一个假 IP 就能骗过去。
    run(
        [](auto& server)
        {
            server.set_trusted_proxies({ "127.0.0.1" });
            echo_client_ip(server);
        },
        [](auto& client) -> net::awaitable<void>
        {
            // 203.0.113.1 是客户端自造的；203.0.113.9 才是最后一跳看到的对端。
            co_await expect_client_ip(client, "203.0.113.1, 203.0.113.9", "203.0.113.9");
        });
}

TEST_CASE("Client IP: falls back to the peer address when the XFF list is unusable", "[server]")
{
    run(
        [](auto& server)
        {
            server.set_trusted_proxies({ "127.0.0.1" });
            echo_client_ip(server);
        },
        [](auto& client) -> net::awaitable<void>
        {
            // 无法解析的项被跳过；全都不合法则退回对端地址。
            co_await expect_client_ip(client, "not-an-ip", "127.0.0.1");
            // 全是可信代理 → 找不到非可信跳板，退回对端地址。
            co_await expect_client_ip(client, "127.0.0.1", "127.0.0.1");
        });
}

TEST_CASE("Client IP: missing X-Forwarded-For yields the peer address", "[server]")
{
    run(
        [](auto& server)
        {
            server.set_trusted_proxies({ "127.0.0.1" });
            echo_client_ip(server);
        },
        [](auto& client) -> net::awaitable<void> { co_await expect_no_xff(client, "127.0.0.1"); });
}

TEST_CASE("Client IP: trusted proxy matching works for IPv6", "[server]")
{
    boost::system::error_code ec;

    // 服务器监听在 v4 回环上，直连对端不是 v6；这里直接验证匹配逻辑。
    REQUIRE(httplib::server::trusted_proxies({ "::1/128" }).contains(net::ip::make_address("::1", ec)));
    REQUIRE_FALSE(httplib::server::trusted_proxies({ "::1/128" }).contains(net::ip::make_address("::2", ec)));

    REQUIRE(httplib::server::trusted_proxies({ "fd00::/8" }).contains(net::ip::make_address("fd12:3456::1", ec)));
    REQUIRE_FALSE(httplib::server::trusted_proxies({ "fd00::/8" }).contains(net::ip::make_address("2001:db8::1", ec)));

    // v4 CIDR 不应命中 v6 地址，反之亦然。
    REQUIRE_FALSE(httplib::server::trusted_proxies({ "127.0.0.0/8" }).contains(net::ip::make_address("::1", ec)));
    REQUIRE_FALSE(httplib::server::trusted_proxies({ "fd00::/8" }).contains(net::ip::make_address("127.0.0.1", ec)));

    // 带 v4 与 v6 的混合列表。
    auto mixed = httplib::server::trusted_proxies({ "10.0.0.0/8", "fd00::/8" });
    REQUIRE(mixed.contains(net::ip::make_address("10.1.2.3", ec)));
    REQUIRE(mixed.contains(net::ip::make_address("fd00::abcd", ec)));
    REQUIRE_FALSE(mixed.contains(net::ip::make_address("11.0.0.1", ec)));
}

TEST_CASE("Client IP: invalid trusted proxy entries are rejected", "[server]")
{
    REQUIRE_THROWS_AS(httplib::server::trusted_proxies({ "not-an-ip" }), std::invalid_argument);
    REQUIRE_THROWS_AS(httplib::server::trusted_proxies({ "10.0.0.0/33" }), std::invalid_argument);
    REQUIRE_THROWS_AS(httplib::server::trusted_proxies({ "2001:db8::/129" }), std::invalid_argument);
    REQUIRE_NOTHROW(httplib::server::trusted_proxies({}));
}
