#include "httplib/body/json_body.hpp"
#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/ws_client.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/co_spawn.hpp>
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <memory>
#include <mutex>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

using namespace std::string_view_literals;
namespace http = httplib::http;
namespace net = httplib::net;

namespace
{

    template <typename Setup, typename Test>
    void
    run_ws(Setup&& setup, Test&& test)
    {
        net::thread_pool pool{ 2 };
        std::exception_ptr err;
        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void>
            {
                httplib::server::http_server server(pool.get_executor());
                auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
                server.set_logger(std::make_shared<spdlog::logger>("test", null_sink));

                setup(server);

                server.listen("127.0.0.1", 0);
                auto ep = server.local_endpoint();
                server.run();

                co_await test(pool, ep);

                server.stop();
            },
            [&](std::exception_ptr e)
            {
                err = e;
            });
        pool.join();
        if (err)
            std::rethrow_exception(err);
    }

    struct ws_done
    {
        std::promise<void> p;
        std::future<void> f = p.get_future();

        void
        notify()
        {
            p.set_value();
        }
        void
        wait()
        {
            f.wait();
        }
    };

} // namespace

// ===========================================================================
// Echo
// ===========================================================================

TEST_CASE("websocket: echo server and client", "[websocket]")
{
    run_ws(
        [](auto& server)
        {
            server.router().set_ws_handler(
                "/ws",
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; },
                [](httplib::server::websocket_conn::weak_ptr conn, std::string_view msg,
                   bool binary) -> net::awaitable<void>
                {
                    if (auto c = conn.lock())
                        c->send(msg, binary);
                    co_return;
                },
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            std::vector<std::string> client_received;
            ws_done done;

            httplib::client::ws_client ws(pool.get_executor(), ep.address().to_string(),
                                          ep.port());

            ws.run(
                "/ws",
                [&](boost::system::error_code ec) -> net::awaitable<void>
                {
                    REQUIRE(!ec);
                    ws.send("hello from client");
                    ws.send("another message");
                    co_return;
                },
                [&](std::string_view msg, bool) -> net::awaitable<void>
                {
                    client_received.emplace_back(msg);
                    if (client_received.size() >= 2)
                        ws.close();
                    co_return;
                },
                [&]() -> net::awaitable<void>
                {
                    done.notify();
                    co_return;
                });

            done.wait();

            REQUIRE(client_received.size() == 2);
            REQUIRE(client_received[0] == "hello from client");
            REQUIRE(client_received[1] == "another message");
            co_return;
        });
}

// ===========================================================================
// Binary message
// ===========================================================================

TEST_CASE("websocket: binary message", "[websocket]")
{
    run_ws(
        [](auto& server)
        {
            server.router().set_ws_handler(
                "/ws-bin",
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; },
                [](httplib::server::websocket_conn::weak_ptr conn, std::string_view msg,
                   bool binary) -> net::awaitable<void>
                {
                    if (auto c = conn.lock())
                        c->send(msg, binary);
                    co_return;
                },
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            bool response_binary = false;
            ws_done done;

            httplib::client::ws_client ws(pool.get_executor(), ep.address().to_string(),
                                          ep.port());

            ws.run(
                "/ws-bin",
                [&](boost::system::error_code) -> net::awaitable<void>
                {
                    std::string data(4, '\x00');
                    data[0] = '\x01';
                    data[1] = '\x02';
                    data[2] = '\x03';
                    data[3] = '\x04';
                    ws.send(std::move(data), true);
                    co_return;
                },
                [&](std::string_view, bool binary) -> net::awaitable<void>
                {
                    response_binary = binary;
                    ws.close();
                    co_return;
                },
                [&]() -> net::awaitable<void>
                {
                    done.notify();
                    co_return;
                });

            done.wait();

            REQUIRE(response_binary);
            co_return;
        });
}

// ===========================================================================
// Close propagation
// ===========================================================================

TEST_CASE("websocket: close propagates to server", "[websocket]")
{
    run_ws(
        [](auto& server)
        {
            server.router().set_ws_handler(
                "/ws-close",
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; },
                [](httplib::server::websocket_conn::weak_ptr conn, std::string_view,
                   bool) -> net::awaitable<void>
                {
                    if (auto c = conn.lock())
                        c->close();
                    co_return;
                },
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            bool client_close_called = false;
            ws_done done;

            httplib::client::ws_client ws(pool.get_executor(), ep.address().to_string(),
                                          ep.port());

            ws.run(
                "/ws-close",
                [&](boost::system::error_code) -> net::awaitable<void>
                {
                    ws.send("close-me");
                    co_return;
                },
                [](std::string_view, bool) -> net::awaitable<void> { co_return; },
                [&]() -> net::awaitable<void>
                {
                    client_close_called = true;
                    done.notify();
                    co_return;
                });

            done.wait();

            REQUIRE(client_close_called);
            co_return;
        });
}

// ===========================================================================
// Ping
// ===========================================================================

TEST_CASE("websocket: client ping", "[websocket]")
{
    run_ws(
        [](auto& server)
        {
            server.router().set_ws_handler(
                "/ws-ping",
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; },
                [](httplib::server::websocket_conn::weak_ptr, std::string_view,
                   bool) -> net::awaitable<void> { co_return; },
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            bool ping_ok = false;
            ws_done done;

            httplib::client::ws_client ws(pool.get_executor(), ep.address().to_string(),
                                          ep.port());

            ws.run(
                "/ws-ping",
                [&](boost::system::error_code ec) -> net::awaitable<void>
                {
                    REQUIRE(!ec);
                    auto result = co_await ws.async_ping("hello");
                    ping_ok = !result;
                    ws.close();
                    co_return;
                },
                [](std::string_view, bool) -> net::awaitable<void> { co_return; },
                [&]() -> net::awaitable<void>
                {
                    done.notify();
                    co_return;
                });

            done.wait();

            REQUIRE(ping_ok);
            co_return;
        });
}

// ===========================================================================
// Close with reason
// ===========================================================================

TEST_CASE("websocket: close with reason", "[websocket]")
{
    run_ws(
        [](auto& server)
        {
            server.router().set_ws_handler(
                "/ws-close-reason",
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; },
                [](httplib::server::websocket_conn::weak_ptr, std::string_view,
                   bool) -> net::awaitable<void> { co_return; },
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            bool client_close_called = false;
            ws_done done;

            httplib::client::ws_client ws(pool.get_executor(), ep.address().to_string(),
                                          ep.port());

            ws.run(
                "/ws-close-reason",
                [&](boost::system::error_code) -> net::awaitable<void>
                {
                    ws.close();
                    co_return;
                },
                [](std::string_view, bool) -> net::awaitable<void> { co_return; },
                [&]() -> net::awaitable<void>
                {
                    client_close_called = true;
                    done.notify();
                    co_return;
                });

            done.wait();

            REQUIRE(client_close_called);
            co_return;
        });
}

// ===========================================================================
// is_open
// ===========================================================================

TEST_CASE("websocket: is_open flag", "[websocket]")
{
    run_ws(
        [](auto& server)
        {
            server.router().set_ws_handler(
                "/ws-is-open",
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; },
                [](httplib::server::websocket_conn::weak_ptr conn, std::string_view,
                   bool) -> net::awaitable<void>
                {
                    if (auto c = conn.lock())
                        c->send(std::string_view("pong"), false);
                    co_return;
                },
                [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
                { co_return; });
        },
        [](auto& pool, auto& ep) -> net::awaitable<void>
        {
            ws_done done;

            httplib::client::ws_client ws(pool.get_executor(), ep.address().to_string(),
                                          ep.port());

            ws.run(
                "/ws-is-open",
                [&](boost::system::error_code) -> net::awaitable<void>
                {
                    ws.send("ping");
                    co_return;
                },
                [&](std::string_view, bool) -> net::awaitable<void>
                {
                    ws.close();
                    co_return;
                },
                [&]() -> net::awaitable<void>
                {
                    done.notify();
                    co_return;
                });

            done.wait();
            co_return;
        });
}
