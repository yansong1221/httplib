// =============================================================================
// P0 设计审查验证用例（DESIGN_REVIEW.md）
//
// 每个问题写成"断言期望（修复后）的行为"，当前代码下预期失败：
//   * 失败 ⇒ 证实该缺陷存在（这是本文件的用途）；修复后 ⇒ 通过，转为回归测试。
// 每个问题都带一组 control 正例，证明脚手架/断言本身可靠，而非测试写错。
//
// 覆盖（SERVER/CLIENT 行为类）：
//   P0-3  WS 升级请求路由不命中 ⇒ 应回 4xx HTTP 响应（当前：TCP 直接关闭，零字节）
//   P0-4  流式响应中途失败 ⇒ 连接必须标脏（当前：keep-alive 继续，下一条响应头
//         会被客户端当作上一条 chunked 响应 body 的延续 —— 协议 desync）
//   P0-5  cors_middleware 多 origin 配置静默失效（含 * + credentials 组合）
//
// 注意：与既有测试同目录，TU 隔离规则（CMakeLists.txt 注释）适用 —— 本文件
// 辅助符号均在匿名 namespace 且带 p0_ 前缀。
// =============================================================================
#include "common.hpp"
#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/stream_writer.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "httplib/websocket_message.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/steady_timer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <string>

using namespace std::string_view_literals;
namespace net = httplib::net;
namespace mw = httplib::server::middleware;

using test_common::run;

namespace
{

    template <typename Setup, typename Exchange>
    void
    run_with_ep(Setup&& setup, Exchange&& exchange)
    {
        net::thread_pool pool { 1 };
        std::exception_ptr err;

        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void>
            {
                httplib::server::http_server server(pool.get_executor());
                test_common::setup_logger(server);
                setup(server);
                server.listen("127.0.0.1", 0);
                auto ep = server.local_endpoint();
                server.run();

                co_await exchange(ep);

                server.stop();
            },
            [&](std::exception_ptr e) { err = e; });

        pool.join();
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

    struct p0_raw_result
    {
        std::string bytes;
    };

    /// 裸 TCP 连接，一次性写出 payload，读到 EOF/对端关闭/看门狗超时为止。
    net::awaitable<p0_raw_result>
    p0_raw_exchange(httplib::tcp::endpoint ep, std::string_view payload, std::chrono::milliseconds budget)
    {
        auto ex = co_await net::this_coro::executor;
        net::ip::tcp::socket sock { ex };
        net::steady_timer watchdog { ex, budget };
        char buf[8192];
        p0_raw_result out;

        watchdog.async_wait([&](boost::system::error_code ec)
                            {
                                if (!ec)
                                {
                                    boost::system::error_code close_ec;
                                    sock.close(close_ec);
                                }
                            });

        co_await sock.async_connect(ep, net::use_awaitable);
        co_await net::async_write(sock, net::buffer(payload), net::use_awaitable);
        boost::system::error_code sec;
        sock.shutdown(net::socket_base::shutdown_send, sec);

        for (;;)
        {
            boost::system::error_code ec;
            auto n = co_await sock.async_read_some(net::buffer(buf), httplib::util::net_awaitable[ec]);
            if (ec)
            {
                // EOF / watchdog 触发的 close（operation_aborted）都是正常退出。
                break;
            }
            out.bytes.append(buf, n);
        }

        // 若 EOF 先到，读循环提前退出；这里 cancel 并**消费掉 pending 的等待**，
        // 确保回调在栈上对象（sock）被销毁前已运行完毕。
        watchdog.cancel();
        co_await watchdog.async_wait(net::use_awaitable);
        co_return out;
    }

    /// RFC 6455 §4.2 的 WebSocket 升级请求（未注册路由的路径）。
    std::string
    p0_ws_handshake(std::string_view path)
    {
        std::string req;
        req.reserve(256);
        req.append("GET ").append(path).append(" HTTP/1.1\r\n");
        req.append("Host: localhost\r\n");
        req.append("Upgrade: websocket\r\n");
        req.append("Connection: Upgrade\r\n");
        req.append("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
        req.append("Sec-WebSocket-Version: 13\r\n\r\n");
        return req;
    }

} // namespace

// ===========================================================================
// P0-3：WS 升级请求在路由不命中时应返回 4xx HTTP 响应
//   现状（websocket_conn_impl.cpp:218-222）：query_ws_handler 未命中 ⇒ co_return，
//   不发任何 HTTP 响应，TCP 直接关闭 —— 客户端看到的是晦涩的 connection closed。
//   期望：普通 HTTP 语义生效（RFC 6455 §4.2 要求不可升级时回 4xx）。
// ===========================================================================

TEST_CASE("P0-3: WS upgrade to a route without handler must get a 4xx HTTP response",
          "[p0][ws][expected-fail]")
{
    run_with_ep(
        [](auto& server)
        {
            // 注册一个合法 WS 路由，确保"其它路径的升级请求"走到未命中分支。
            server.router().set_ws_handler(
                "/ws",
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
                [](httplib::server::websocket_conn::weak_ptr,
                   httplib::websocket_message) -> net::awaitable<void> { co_return; },
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });
        },
        [](auto ep) -> net::awaitable<void>
        {
            auto result
                = co_await p0_raw_exchange(ep, p0_ws_handshake("/no-such-ws"), std::chrono::seconds { 2 });

            // 期望（修复后）：对端至少回一个 4xx 状态行的 HTTP 响应。
            // 当前（缺陷）：bytes 为空 —— 服务端零字节关闭。
            INFO("P0-3 缺陷证据：升级请求未命中 WS 路由时服务端未发送任何 HTTP 响应（bytes 截取: "
                 << (result.bytes.size() > 80 ? result.bytes.substr(0, 80) : result.bytes) << ")");
            REQUIRE(!result.bytes.empty());
            REQUIRE(result.bytes.starts_with("HTTP/1.1 4"));
        });
}

// ===========================================================================
// P0-4：流式响应中途失败时连接必须标脏（不允许继续 keep-alive）
//   现状（reverse_proxy_impl.cpp / stream_writer_impl.cpp / session.cpp）：
//   handler 经 stream_writer 写出 200 + 部分分节后抛异常，session 不给连接
//   标脏，继续在同一个连接上服务下一条请求。
//   后果（协议 desync）：下一条响应头出现在上一条 chunked 响应 body 的延续
//   位置上，客户端把第二个响应头解析成分块数据。
//   期望：第一条响应未正常终结 （无 0\r\n\r\n terminator） ⇒ 连接必须被关闭，
//            后续请求不可能在同一连接上继续被应答。
// ===========================================================================

TEST_CASE("P0-4: failed mid-stream response must not be followed by another response on the same connection",
          "[p0][stream][expected-fail]")
{
    run_with_ep(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/half",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.create_stream_writer();
                    httplib::headers headers;
                    headers.set(httplib::field::content_type, "text/plain");
                    co_await cw->write_header(httplib::status::ok,
                                              headers,
                                              httplib::server::stream_writer::mode::chunked);
                    // 只写一节、more=true，然后异常退出 => 没有 0\r\n\r\n 终结符。
                    co_await cw->write_body(net::buffer("AAAA", 4), true);
                    throw std::runtime_error("p0-4: handler failed mid-stream");
                });
            server.router().template set_http_handler<httplib::method::get>(
                "/after",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); });
        },
        [](auto ep) -> net::awaitable<void>
        {
            // 两条请求在同一连接上背靠背发出（管道化），然后读到 EOF/超时。
            std::string pipelined;
            pipelined += "GET /half HTTP/1.1\r\nHost: localhost\r\nAccept: */*\r\n\r\n";
            pipelined += "GET /after HTTP/1.1\r\nHost: localhost\r\nAccept: */*\r\n\r\n";
            auto result = co_await p0_raw_exchange(ep, pipelined, std::chrono::seconds { 2 });

            constexpr std::string_view chunk_probe = "4\r\nAAAA\r\n";
            constexpr std::string_view terminator = "0\r\n\r\n";

            INFO("P0-4 缺陷证据：同连接的第二个响应头出现在第一条 chunked 响应 body 的延续位置上"
                 "（原始流截取: "
                 << (result.bytes.size() > 120 ? result.bytes.substr(0, 120) : result.bytes) << ")");

            auto sec = std::string_view { result.bytes };
            auto stream_chunk = sec.find(chunk_probe);
            REQUIRE(stream_chunk != std::string_view::npos);

            auto search_from = stream_chunk + chunk_probe.size();
            auto next_status = sec.find("HTTP/1.", search_from);
            auto term = sec.find(terminator, search_from);

            bool desync = next_status != std::string_view::npos && (term == std::string_view::npos ||
                                                                    next_status < term);
            // 期望：要么第一个连接被关闭（EOF 且没有第二个状态行），要么 body
            // 完整终结（在出现第二个状态行之前有 0\r\n\r\n）。
            REQUIRE(!desync);
        });
}

// ===========================================================================
// P0-5：cors_middleware
//   a) 多 origin 白名单：请求的 Origin 命中白名单 ⇒ 应回显（reflect）该 Origin
//      并加 Vary: Origin；当前（cors.cpp:34-46）>1 个 origin 时返回空串 ⇒
//      完全不回 ACAO —— 资源可被其它 origin 上的 JS 消费（预取失败）。
//   b) allow_credentials(true) + 默认 origins="*"：ACAO:* 与 ACAC:true 同屏
//      发送（违反 Fetch 规范）。
//   control 组：
//      单 allow_origin 时的响应是正确的（该路径当前逻辑正常）→ 证明断言可靠。
// ===========================================================================

TEST_CASE("P0-5: cors_middleware should reflect a whitelisted Origin from a multi-origin list",
          "[p0][cors][expected-fail]")
{
    mw::cors_middleware cors;
    cors.allow_origins({ "https://a.com", "https://b.com" });

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/p0-cors-multi",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("cors"sv, "text/plain"sv); },
                cors);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            httplib::headers hdrs;
            hdrs.set(httplib::field::origin, "https://a.com");
            httplib::client::request req(httplib::method::get, "/p0-cors-multi", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == httplib::status::ok);

            // 期望（修复后）： ACAO 应等于命中的白名单 origin；同时带 Vary: Origin。
            // 当前（缺陷）：ACAO 为空 —— 吞掉。
            INFO("P0-5 缺陷证据：多 origin 白名单命中后响应仍无 "
                 "Access-Control-Allow-Origin，跨源 JS 无法读取");
            REQUIRE_FALSE(resp["Access-Control-Allow-Origin"].empty());
            REQUIRE(resp["Access-Control-Allow-Origin"] == "https://a.com");
            REQUIRE_FALSE(resp["Vary"].empty());
        });
}

TEST_CASE("P0-5: cors_middleware must not emit '*' alongside credentials",
          "[p0][cors][expected-fail]")
{
    // control：未配置 credentials，* 是合法的
    mw::cors_middleware anonymous;
    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/p0-cors-wild",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("wild"sv, "text/plain"sv); },
                anonymous);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            httplib::headers hdrs;
            hdrs.set(httplib::field::origin, "https://x.com");
            httplib::client::request req(httplib::method::get, "/p0-cors-wild", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            INFO("control：组0 — 无 credentials 时 ACAO:* 是合法的");
            REQUIRE(resp["Access-Control-Allow-Origin"] == "*");
            REQUIRE(resp["Access-Control-Allow-Credentials"].empty());
        });

    // 缺陷：credentials(true) + 默认 * —— 运行时求解星形 vs credentialed 可协商性
    mw::cors_middleware credentialed;
    credentialed.allow_credentials(true);
    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/p0-cors-cred",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("cred"sv, "text/plain"sv); },
                credentialed);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            httplib::headers hdrs;
            hdrs.set(httplib::field::origin, "https://x.com");
            httplib::client::request req(httplib::method::get, "/p0-cors-cred", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));

            // 期望（修复后）：ACAO:*（可见的）与 ACAC:true 不同屏。
            INFO("P0-5 缺陷证据：ACAO:'* 与 ACAC:true 同屏（Fetch 规范禁止；浏览器将拒绝）");
            if (resp["Access-Control-Allow-Credentials"] == "true")
            {
                REQUIRE_FALSE(resp["Access-Control-Allow-Origin"] == "*");
            }
        });
}

TEST_CASE("control: cors_middleware single allow_origin works (assertions are trustworthy)",
          "[p0][cors][control][do-not-fail]")
{
    mw::cors_middleware cors;
    cors.allow_origin("https://only.com");

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/p0-cors-ctrl",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ctrl"sv, "text/plain"sv); },
                cors);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            httplib::headers hdrs;
            hdrs.set(httplib::field::origin, "https://only.com");
            httplib::client::request req(httplib::method::get, "/p0-cors-ctrl", hdrs);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            INFO("control：单 origin 正常回 ACAO —— 证明本文件断言/脚手架可靠");
            REQUIRE(resp["Access-Control-Allow-Origin"] == "https://only.com");
        });
}
