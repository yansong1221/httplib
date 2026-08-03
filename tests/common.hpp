#pragma once
#include "httplib/client/client.hpp"
#include "httplib/client/read_session.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>

using namespace std::string_view_literals;
namespace http = httplib::http;
namespace net = httplib::net;

namespace test_common
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

        void
        start_with_long_timeout()
        {
            server.listen("127.0.0.1", 0);
            endpoint = server.local_endpoint();
            server.run();
            thread = std::thread([this] { ioc.run(); });
            started_ = true;

            client = std::make_unique<httplib::client::http_client>(ioc.get_executor(),
                                                                    endpoint.address().to_string(),
                                                                    endpoint.port());
            client->set_timeout(std::chrono::seconds(30));
        }

        auto&
        router()
        {
            return server.router();
        }
    };

    template <typename R>
    std::string
    as_string(R const& resp)
    {
        return resp.body().as<httplib::body::string_body>();
    }

} // namespace test_common

inline net::awaitable<void>
collect_sse_events(httplib::client::sse_reader& sse, std::vector<httplib::client::sse_reader::sse_event>& events)
{
    while (!sse.is_done())
    {
        auto result = co_await sse.read_event();
        if (result.has_error())
        {
            break;
        }
        auto& ev = result.value();
        if (ev.data.empty() && ev.id.empty() && ev.event.empty() && ev.retry == std::chrono::milliseconds { 0 })
        {
            break;
        }
        events.push_back(std::move(ev));
    }
}

inline net::awaitable<void>
collect_ndjson_lines(httplib::client::ndjson_reader& ndjson, std::vector<boost::json::value>& items)
{
    while (!ndjson.is_done())
    {
        auto result = co_await ndjson.read();
        if (result.has_error())
        {
            break;
        }
        auto& val = result.value();
        if (val.is_null())
        {
            break;
        }
        items.push_back(std::move(val));
    }
}

#define UNWRAP(result)               \
    [&](auto&& r)                    \
    {                                \
        REQUIRE(r.has_value());      \
        return std::move(r).value(); \
    }(result)
