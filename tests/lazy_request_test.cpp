#include "body/form_data_body.hpp"
#include "body/json_body.hpp"
#include "body/query_params_body.hpp"
#include "body/string_body.hpp"
#include "common.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <array>
#include <boost/json/serialize.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace body = httplib::body;
namespace html = httplib::html;
namespace net = httplib::net;
namespace http = httplib::http;

namespace
{
    using test_common::run;

    std::string
    big_payload()
    {
        std::string data;
        data.reserve(2 * 1024 * 1024);
        for (int i = 0; i < 2 * 1024 * 1024; ++i)
        {
            data.push_back(static_cast<char>('a' + (i % 26)));
        }
        return data;
    }

} // namespace

TEST_CASE("server lazy: read_string", "[server-lazy]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/string",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(req.is_lazy());
                    auto data = UNWRAP(co_await req.read_string());
                    resp.set_string_content(std::move(data), "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/string", std::string_view("hello lazy")));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.as_string() == "hello lazy");
        });
}

TEST_CASE("server lazy: read_json", "[server-lazy]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/json",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto result = co_await req.read_json();
                    if (result.has_error())
                    {
                        resp.set_string_content(std::string("ERR:") + result.error().message(), "text/plain");
                        co_return;
                    }
                    auto value = result.value();
                    spdlog::info("server json value kind={} is_obj={} dump={}",
                                 (int)value.kind(),
                                 value.is_object(),
                                 boost::json::serialize(value));
                    if (!value.is_object())
                    {
                        resp.set_string_content(std::string("NOT-OBJECT"), "text/plain");
                        co_return;
                    }
                    resp.set_json_content(value);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/json", boost::json::value({{"msg","hi"}})));
            REQUIRE(resp.result() == http::status::ok);
            spdlog::info("client json ct={} body={}",
                         std::string(resp.base()[http::field::content_type]),
                         boost::json::serialize(resp.as_json()));
            REQUIRE(resp.as_json().at("msg").as_string() == "hi");
        });
}

TEST_CASE("server lazy: read_body default content-type dispatch", "[server-lazy]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/any",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    UNWRAP(co_await req.read_body());
                    // content-type: application/json 触发 json 派发，body 类型为 json_body
                    REQUIRE_NOTHROW(req.as_json());
                    auto const& value = req.as_json();
                    resp.set_json_content(value);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/any", boost::json::value({{"a",1}})));
            REQUIRE(resp.result() == http::status::ok);
        });
}

TEST_CASE("server lazy: read_query_params", "[server-lazy]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/params",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto qp = UNWRAP(co_await req.read_query_params());
                    resp.set_string_content(std::string(qp.at("key")), "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            html::query_params params;
            params.add("key", "url-value");
            auto req = httplib::client::request(http::verb::post, "/params");
            req.set_body(std::move(params));
            auto resp = UNWRAP(co_await client.async_send_request(std::move(req)));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.as_string() == "url-value");
        });
}

TEST_CASE("server lazy: read_some_raw streaming", "[server-lazy]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/raw",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    std::string received;
                    std::array<char, 64 * 1024> buf;
                    for (;;)
                    {
                        auto result = co_await req.read_some_raw(net::buffer(buf));
                        if (result.has_error())
                        {
                            resp.set_string_content(std::string("ERR:") + result.error().message(), "text/plain");
                            co_return;
                        }
                        auto n = result.value();
                        if (n == 0)
                        {
                            break;
                        }
                        received.append(buf.data(), n);
                    }
                    REQUIRE(req.is_body_done());
                    resp.set_string_content(std::move(received), "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto big = big_payload();
            auto resp = UNWRAP(co_await client.async_post("/raw", std::string_view(big)));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.as_string() == big);
        });
}

TEST_CASE("server lazy: body not consumed forces connection close", "[server-lazy]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/ignore",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    REQUIRE(!req.is_body_done());
                    resp.set_string_content(std::string("ok"), "text/plain");
                    co_return;
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(
                co_await client.async_post("/ignore", std::string_view("a request body that is never consumed")));
            REQUIRE(resp.result() == http::status::ok);
            // 服务端已发 Connection: close，客户端下一请求应自动重连并成功
            auto resp2 = UNWRAP(co_await client.async_post("/ignore", std::string_view("second")));
            REQUIRE(resp2.result() == http::status::ok);
        });
}

TEST_CASE("server lazy: regular handler takes precedence", "[server-lazy]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/both",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(!req.is_lazy());
                    resp.set_string_content(std::string("regular"), "text/plain");
                });
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/both",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    resp.set_string_content(std::string("lazy"), "text/plain");
                    co_return;
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/both", std::string_view("body")));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.as_string() == "regular");
        });
}

TEST_CASE("server lazy: read_form_data with file upload", "[server-lazy]")
{
    run(
        [](auto& server)
        {
            server.router().template set_lazy_http_handler<http::verb::post>(
                "/upload",
                [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto fd = UNWRAP(co_await req.read_form_data());
                    auto fld = fd.field_by_name("file");
                    if (!fld || fld->content.empty())
                    {
                        resp.set_string_content(std::string("missing"), "text/plain");
                        co_return;
                    }
                    resp.set_string_content(fld->content, "text/plain");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            html::form_data form;
            form.boundary = "----TestFormBoundary";
            form.fields.push_back({ "file", "data.txt", "text/plain", "file-contents" });
            auto req = httplib::client::request(http::verb::post, "/upload");
            req.set_body(std::move(form));
            auto resp = UNWRAP(co_await client.async_send_request(std::move(req)));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.as_string() == "file-contents");
        });
}