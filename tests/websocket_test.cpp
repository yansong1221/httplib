#include "httplib/body/json_body.hpp"
#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/ws_client.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/thread_pool.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
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

    struct ws_test_server
    {
        net::thread_pool ioc{2};
        httplib::server::http_server server;
        httplib::tcp::endpoint endpoint;

        ws_test_server() : server(ioc.get_executor())
        {
            auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            server.set_logger(std::make_shared<spdlog::logger>("httplib.tests", null_sink));
        }

        ~ws_test_server()
        {
            server.stop();
            ioc.join();
        }

        void
        start()
        {
            server.listen("127.0.0.1", 0);
            endpoint = server.local_endpoint();
            server.run();
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

} // namespace

TEST_CASE("WebSocket echo server and client", "[websocket]")
{
    ws_test_server server;
    std::vector<std::string> server_received;
    std::mutex srv_mutex;

    server.router().set_ws_handler(
        "/ws",
        [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [&](httplib::server::websocket_conn::weak_ptr conn, std::string_view msg, bool binary) -> net::awaitable<void>
        {
            {
                std::lock_guard lock(srv_mutex);
                server_received.emplace_back(msg);
            }
            auto c = conn.lock();
            if (c)
            {
                c->send(msg, binary);
            }
            co_return;
        },
        [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });
    server.start();

    net::thread_pool client_ioc;
    httplib::client::ws_client ws_client(client_ioc.get_executor(), server.host(), server.port());
    std::vector<std::string> client_received;
    std::mutex cli_mutex;
    bool open_called = false;

    ws_client.run(
        "/ws",
        [&](boost::system::error_code ec) -> net::awaitable<void>
        {
            REQUIRE(!ec);
            open_called = true;
            ws_client.send("hello from client");
            ws_client.send("another message");
            co_return;
        },
        [&](std::string_view msg, bool) -> net::awaitable<void>
        {
            std::lock_guard lock(cli_mutex);
            client_received.emplace_back(msg);
            if (client_received.size() >= 2)
            {
                ws_client.close();
            }
            co_return;
        },
        []() -> net::awaitable<void> { co_return; });
    std::this_thread::sleep_for(std::chrono::seconds(5));
    server.server.stop().wait();
    client_ioc.join();

    REQUIRE(open_called);
    REQUIRE(client_received.size() >= 2);
    REQUIRE(client_received.at(0) == "hello from client");
    REQUIRE(client_received.at(1) == "another message");
}

TEST_CASE("WebSocket binary message", "[websocket]")
{
    ws_test_server server;
    bool received_binary = false;
    std::string received_content;

    server.router().set_ws_handler(
        "/ws-bin",
        [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [&](httplib::server::websocket_conn::weak_ptr conn, std::string_view msg, bool binary) -> net::awaitable<void>
        {
            received_binary = binary;
            received_content = msg;
            auto c = conn.lock();
            if (c)
            {
                c->send(msg, binary);
            }
            co_return;
        },
        [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });
    server.start();

    net::thread_pool client_ioc;
    httplib::client::ws_client ws_client(client_ioc.get_executor(), server.host(), server.port());
    bool response_received = false;
    bool response_binary = false;

    ws_client.run(
        "/ws-bin",
        [&](boost::system::error_code) -> net::awaitable<void>
        {
            std::string binary_data(4, '\x00');
            binary_data[0] = '\x01';
            binary_data[1] = '\x02';
            binary_data[2] = '\x03';
            binary_data[3] = '\x04';
            ws_client.send(std::move(binary_data), true);
            co_return;
        },
        [&](std::string_view, bool binary) -> net::awaitable<void>
        {
            response_received = true;
            response_binary = binary;
            ws_client.close();
            co_return;
        },
        []() -> net::awaitable<void> { co_return; });
    std::this_thread::sleep_for(std::chrono::seconds(5));
    server.server.stop().wait();
    client_ioc.join();

    REQUIRE(received_binary);
    REQUIRE(received_content.size() == 4);
    REQUIRE(static_cast<unsigned char>(received_content[0]) == 0x01);
    REQUIRE(static_cast<unsigned char>(received_content[1]) == 0x02);
    REQUIRE(response_received);
    REQUIRE(response_binary);
}

TEST_CASE("WebSocket close propagates to server", "[websocket]")
{
    ws_test_server server;
    bool server_close_called = false;

    server.router().set_ws_handler(
        "/ws-close",
        [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [&](httplib::server::websocket_conn::weak_ptr conn, std::string_view, bool) -> net::awaitable<void>
        {
            auto c = conn.lock();
            if (c)
            {
                c->close();
            }
            co_return;
        },
        [&](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
        {
            server_close_called = true;
            co_return;
        });
    server.start();

    net::thread_pool client_ioc;
    httplib::client::ws_client ws_client(client_ioc.get_executor(), server.host(), server.port());
    bool client_close_called = false;

    ws_client.run(
        "/ws-close",
        [&](boost::system::error_code) -> net::awaitable<void>
        {
            ws_client.send("close-me");
            co_return;
        },
        [](std::string_view, bool) -> net::awaitable<void> { co_return; },
        [&]() -> net::awaitable<void>
        {
            client_close_called = true;
            co_return;
        });
    std::this_thread::sleep_for(std::chrono::seconds(5));
    server.server.stop().wait();
    client_ioc.join();

    REQUIRE(server_close_called);
    REQUIRE(client_close_called);
}

TEST_CASE("WebSocket client ping", "[websocket]")
{
    ws_test_server server;
    std::atomic<bool> ping_received { false };

    server.router().set_ws_handler(
        "/ws-ping",
        [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [&](httplib::server::websocket_conn::weak_ptr, std::string_view, bool) -> net::awaitable<void> { co_return; },
        [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });
    server.start();

    net::thread_pool client_ioc;
    httplib::client::ws_client ws_client(client_ioc.get_executor(), server.host(), server.port());
    bool ping_result = false;

    ws_client.run(
        "/ws-ping",
        [&](boost::system::error_code ec) -> net::awaitable<void>
        {
            REQUIRE(!ec);
            auto result = co_await ws_client.async_ping("hello");
            ping_result = !result;
            ws_client.close();
            co_return;
        },
        [](std::string_view, bool) -> net::awaitable<void> { co_return; },
        []() -> net::awaitable<void> { co_return; });

    std::this_thread::sleep_for(std::chrono::seconds(5));
    server.server.stop().wait();
    client_ioc.join();

    REQUIRE(ping_result);
}

TEST_CASE("WebSocket close with reason", "[websocket]")
{
    ws_test_server server;
    std::string close_reason_received;

    server.router().set_ws_handler(
        "/ws-close-reason",
        [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; },
        [&](httplib::server::websocket_conn::weak_ptr, std::string_view, bool) -> net::awaitable<void> { co_return; },
        [&](httplib::server::websocket_conn::weak_ptr conn) -> net::awaitable<void>
        {
            auto c = conn.lock();
            if (c)
            {
                close_reason_received = "server-closed";
            }
            co_return;
        });
    server.start();

    net::thread_pool client_ioc;
    httplib::client::ws_client ws_client(client_ioc.get_executor(), server.host(), server.port());
    bool client_close_called = false;

    ws_client.run(
        "/ws-close-reason",
        [&](boost::system::error_code) -> net::awaitable<void>
        {
            ws_client.close();
            co_return;
        },
        [](std::string_view, bool) -> net::awaitable<void> { co_return; },
        [&]() -> net::awaitable<void>
        {
            client_close_called = true;
            co_return;
        });
    client_ioc.join();

    REQUIRE(client_close_called);
    REQUIRE(close_reason_received == "server-closed");
}

TEST_CASE("WebSocket is_open flag", "[websocket]")
{
    ws_test_server server;
    bool open_is_open = false;
    bool msg_is_open = false;

    server.router().set_ws_handler(
        "/ws-is-open",
        [&](httplib::server::websocket_conn::weak_ptr conn) -> net::awaitable<void>
        {
            auto c = conn.lock();
            if (c)
            {
                open_is_open = c->is_open();
            }
            co_return;
        },
        [&](httplib::server::websocket_conn::weak_ptr conn, std::string_view, bool) -> net::awaitable<void>
        {
            auto c = conn.lock();
            if (c)
            {
                msg_is_open = c->is_open();
            }
            co_return;
        },
        [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });
    server.start();

    net::thread_pool client_ioc;
    httplib::client::ws_client ws_client(client_ioc.get_executor(), server.host(), server.port());
    bool send_done = false;

    ws_client.run(
        "/ws-is-open",
        [&](boost::system::error_code) -> net::awaitable<void>
        {
            ws_client.send("ping");
            send_done = true;
            co_return;
        },
        [&](std::string_view, bool) -> net::awaitable<void>
        {
            ws_client.close();
            co_return;
        },
        []() -> net::awaitable<void> { co_return; });
    std::this_thread::sleep_for(std::chrono::seconds(5));
    server.server.stop().wait();
    client_ioc.join();

    REQUIRE(send_done);
    REQUIRE(open_is_open);
    REQUIRE(msg_is_open);
} // namespace