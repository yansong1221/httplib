#include "common.hpp"
#include "httplib/body/form_data_body.hpp"
#include "httplib/body/json_body.hpp"
#include "httplib/body/query_params_body.hpp"
#include "httplib/body/string_body.hpp"
#include "httplib/html/query_params.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>

namespace body = httplib::body;
namespace http = httplib::http;
namespace net = httplib::net;
using test_common::run;
using test_common::setup_logger;

namespace
{

    auto const&
    as_json(httplib::client::http_client::response const& resp)
    {
        return resp.body().template as<body::json_body>();
    }

    httplib::html::query_params
    params(std::initializer_list<std::pair<std::string, std::string>> vals)
    {
        httplib::html::query_params out;
        for (auto const& [key, val] : vals)
        {
            out.add(key, val);
        }
        return out;
    }

    httplib::http::fields
    headers(std::initializer_list<std::pair<httplib::http::field, std::string>> vals)
    {
        httplib::http::fields out;
        for (auto const& [key, val] : vals)
        {
            out.set(key, val);
        }
        return out;
    }

    void
    set_text(httplib::server::response& resp,
             std::string_view body,
             httplib::http::status status = httplib::http::status::ok)
    {
        resp.set_string_content(body, "text/plain"sv, status);
    }

    auto
    base_headers()
    {
        return headers({
            { httplib::http::field::user_agent, "httplib-test" }
        });
    }

    auto
    query_q1()
    {
        return params({
            { "q", "1" }
        });
    }

} // namespace

TEST_CASE("HTTP GET returns correct body and status", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/method/get",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.method() == http::verb::get);
                    REQUIRE(req.query_params().at("q") == "1");
                    set_text(resp, "get-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp
                = UNWRAP(co_await client.async_get("/method/get", query_q1(), base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "get-ok");
            co_return;
        });
}

TEST_CASE("HTTP HEAD returns correct headers and no body", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::head>(
                "/method/head",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set("X-Head-Test", "head-ok");
                    set_text(resp, "head-body-is-not-sent"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(
                co_await client.async_head("/method/head", query_q1(), base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp[http::field::content_type] == "text/plain");
            REQUIRE(resp["X-Head-Test"] == "head-ok");
            co_return;
        });
}

TEST_CASE("HTTP POST with string body", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/method/post",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.body().template as<body::string_body>() == "post-body");
                    set_text(resp, "post-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post(
                "/method/post", "post-body"sv, query_q1(), base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "post-ok");
            co_return;
        });
}

TEST_CASE("HTTP POST with JSON body", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/method/post-json",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& obj
                        = req.body().template as<body::json_body>().as_object();
                    REQUIRE(obj.at("name").as_string() == "client");
                    resp.set_json_content({
                        {   "ok",           true },
                        { "echo", obj.at("name") }
                    });
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/method/post-json",
                                                          { { "name", "client" } },
                                                          query_q1(),
                                                          base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_json(resp).as_object().at("ok").as_bool());
            REQUIRE(as_json(resp).as_object().at("echo").as_string() == "client");
            co_return;
        });
}

TEST_CASE("HTTP PUT no body", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::put>(
                "/method/put-empty",
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "put-empty-ok"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(
                co_await client.async_put("/method/put-empty", query_q1(), base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "put-empty-ok");
            co_return;
        });
}

TEST_CASE("HTTP PUT with string body", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::put>(
                "/method/put",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.body().template as<body::string_body>() == "put-body");
                    set_text(resp, "put-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_put(
                "/method/put", "put-body"sv, query_q1(), base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "put-ok");
            co_return;
        });
}

TEST_CASE("HTTP PUT with JSON body", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::put>(
                "/method/put-json",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& obj
                        = req.body().template as<body::json_body>().as_object();
                    REQUIRE(obj.at("name").as_string() == "put-json");
                    resp.set_json_content({
                        {     "ok",  true },
                        { "method", "put" }
                    });
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_put("/method/put-json",
                                                         { { "name", "put-json" } },
                                                         query_q1(),
                                                         base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_json(resp).as_object().at("method").as_string() == "put");
            co_return;
        });
}

TEST_CASE("HTTP PATCH with string body", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::patch>(
                "/method/patch",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.body().template as<body::string_body>() == "patch-body");
                    set_text(resp, "patch-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_patch(
                "/method/patch", "patch-body"sv, query_q1(), base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "patch-ok");
            co_return;
        });
}

TEST_CASE("HTTP PATCH with JSON body", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::patch>(
                "/method/patch-json",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& obj
                        = req.body().template as<body::json_body>().as_object();
                    REQUIRE(obj.at("name").as_string() == "patch-json");
                    resp.set_json_content({
                        {     "ok",    true },
                        { "method", "patch" }
                    });
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_patch("/method/patch-json",
                                                           { { "name", "patch-json" } },
                                                           query_q1(),
                                                           base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(as_json(resp).as_object().at("method").as_string() == "patch");
            co_return;
        });
}

TEST_CASE("HTTP DELETE", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::delete_>(
                "/method/delete",
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "delete-ok"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(
                co_await client.async_del("/method/delete", query_q1(), base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "delete-ok");
            co_return;
        });
}

TEST_CASE("HTTP OPTIONS with Allow header", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::options>(
                "/method/options",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set("Allow", "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
                    set_text(resp, "options-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_options(
                "/method/options", query_q1(), base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp[http::field::allow]
                    == "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
            REQUIRE(resp.body().template as<body::string_body>() == "options-ok");
            co_return;
        });
}

TEST_CASE("send_request generic method", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/method/send-request",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.body().template as<body::string_body>()
                            == "generic-body");
                    set_text(resp, "send-request-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/method/send-request", "generic-body"sv, base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "send-request-ok");
            co_return;
        });
}

TEST_CASE("Not found handler returns 404", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().set_http_not_found_handler(
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.path() == "/missing");
                    set_text(resp, "not-found"sv, http::status::not_found);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/missing"));
            REQUIRE(resp.result() == http::status::not_found);
            REQUIRE(resp.body().template as<body::string_body>() == "not-found");
            co_return;
        });
}

TEST_CASE("Client respects user-agent header", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/agent",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.base()[http::field::user_agent] == "custom-agent");
                    set_text(resp, "ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get(
                "/agent",
                {},
                headers({ { http::field::user_agent, "custom-agent" } })));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });
}

TEST_CASE("Multiple query parameters", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/multi-query",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.query_params().at("a") == "1");
                    REQUIRE(req.query_params().at("b") == "hello");
                    REQUIRE(req.query_params().at("c") == "true");
                    set_text(resp, "ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto query = params({
                { "a",     "1" },
                { "b", "hello" },
                { "c",  "true" }
            });
            auto resp = UNWRAP(co_await client.async_get("/multi-query", query));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });
}

TEST_CASE("HTTP TRACE method", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::trace>(
                "/method/trace",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    resp.set("Content-Type", "message/http");
                    resp.set_string_content(
                        std::string(req.base().at(http::field::user_agent)), "text/plain"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_trace(
                "/method/trace", query_q1(), base_headers()));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });
}

TEST_CASE("HTTP Expect: 100-continue header is sent", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/expect-100",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.base()[http::field::expect] == "100-continue");
                    REQUIRE(req.body().template as<body::string_body>()
                            == "large-payload");
                    resp.set_empty_content(http::status::ok);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto hdrs = headers({
                { http::field::expect, "100-continue" }
            });
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/expect-100", "large-payload"sv, hdrs));
            REQUIRE(resp.result() != http::status::bad_request);
            co_return;
        });
}

TEST_CASE("Form-urlencoded body parsing", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/form-encoded",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& params
                        = req.body().template as<body::query_params_body>();
                    auto it_name = params.params().find("name");
                    auto it_value = params.params().find("value");
                    REQUIRE(it_name != params.params().end());
                    REQUIRE(it_value != params.params().end());
                    auto name = it_name->second;
                    auto value = it_value->second;
                    resp.set_string_content(name + "=" + value, "text/plain"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::content_type, "application/x-www-form-urlencoded");
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/form-encoded", "name=foo&value=bar"sv, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "foo=bar");
            co_return;
        });
}

TEST_CASE("Multipart form-data body parsing", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/multipart",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& fd = req.body().template as<body::form_data_body>();
                    std::string result;
                    for (auto const& field : fd.fields)
                    {
                        result += std::format("{}={}", field.name, std::string(field.content));
                        if (field.is_file())
                        {
                            result += std::format(":{}", field.filename);
                        }
                        result += ";";
                    }
                    resp.set_string_content(result, "text/plain"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::string boundary = "----TestBoundary123";
            std::string body = std::format("--{}\r\n"
                                           "Content-Disposition: form-data; name=\"field1\"\r\n"
                                           "\r\n"
                                           "value1\r\n"
                                           "--{}\r\n"
                                           "Content-Disposition: form-data; name=\"file1\"; filename=\"test.txt\"\r\n"
                                           "Content-Type: text/plain\r\n"
                                           "\r\n"
                                           "file-content\r\n"
                                           "--{}--\r\n",
                                           boundary,
                                           boundary,
                                           boundary);

            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::content_type,
                     std::format("multipart/form-data; boundary={}", boundary));
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/multipart", body, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>()
                    == "field1=value1;file1=file-content:test.txt;");
            co_return;
        });
}

TEST_CASE("Multipart file upload saved to disk", "[http-methods]")
{
    auto upload_dir = std::filesystem::temp_directory_path() / "httplib_uploads";
    std::filesystem::create_directories(upload_dir);

    run(
        [&](auto& server)
        {
            server.set_upload_dir(upload_dir);
            server.router().template set_http_handler<http::verb::post>(
                "/upload-disk",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& fd = req.body().template as<body::form_data_body>();
                    REQUIRE(fd.fields.size() == 2);
                    REQUIRE(fd.fields[0].name == "text");
                    REQUIRE(fd.fields[0].content == "hello");
                    REQUIRE(fd.fields[1].name == "file");
                    REQUIRE(fd.fields[1].filename == "upload.bin");
                    REQUIRE(fd.fields[1].file_path.has_value());
                    REQUIRE(fd.fields[1].content.empty());
                    resp.set_string_content("ok"sv, "text/plain"sv);
                });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            std::string boundary = "----DiskUploadBoundary";
            std::string body = std::format("--{}\r\n"
                                           "Content-Disposition: form-data; name=\"text\"\r\n"
                                           "\r\n"
                                           "hello\r\n"
                                           "--{}\r\n"
                                           "Content-Disposition: form-data; name=\"file\"; filename=\"upload.bin\"\r\n"
                                           "Content-Type: application/octet-stream\r\n"
                                           "\r\n"
                                           "binary-content-here\r\n"
                                           "--{}--\r\n",
                                           boundary,
                                           boundary,
                                           boundary);

            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::content_type,
                     std::format("multipart/form-data; boundary={}", boundary));
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/upload-disk", body, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });

    std::filesystem::remove_all(upload_dir);
}

TEST_CASE("Multipart file upload exceeds size limit", "[http-methods]")
{
    auto upload_dir = std::filesystem::temp_directory_path() / "httplib_uploads_limit";
    std::filesystem::create_directories(upload_dir);

    run(
        [&](auto& server)
        {
            server.set_upload_dir(upload_dir);
            server.set_upload_file_limit(4);
            server.router().template set_http_handler<http::verb::post>(
                "/upload-limit",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            std::string boundary = "----LimitBoundary";
            std::string body = std::format("--{}\r\n"
                                           "Content-Disposition: form-data; name=\"file\"; filename=\"big.bin\"\r\n"
                                           "\r\n"
                                           "too-large\r\n"
                                           "--{}--\r\n",
                                           boundary,
                                           boundary);

            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::content_type,
                     std::format("multipart/form-data; boundary={}", boundary));
            auto resp = co_await client.async_send_request(
                http::verb::post, "/upload-limit", body, hdrs);
            REQUIRE_FALSE(resp.has_value());
            co_return;
        });

    std::filesystem::remove_all(upload_dir);
}

TEST_CASE("Multipart multiple files saved to disk", "[http-methods]")
{
    auto upload_dir = std::filesystem::temp_directory_path() / "httplib_uploads_multi";
    std::filesystem::create_directories(upload_dir);

    run(
        [&](auto& server)
        {
            server.set_upload_dir(upload_dir);
            server.router().template set_http_handler<http::verb::post>(
                "/upload-multi",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& fd = req.body().template as<body::form_data_body>();
                    REQUIRE(fd.fields.size() == 2);
                    REQUIRE(fd.fields[0].name == "f1");
                    REQUIRE(fd.fields[0].file_path.has_value());
                    REQUIRE(fd.fields[1].name == "f2");
                    REQUIRE(fd.fields[1].file_path.has_value());
                    REQUIRE(fd.fields[0].file_path != fd.fields[1].file_path);
                    resp.set_string_content("ok"sv, "text/plain"sv);
                });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            std::string boundary = "----MultiBoundary";
            std::string body = std::format("--{}\r\n"
                                           "Content-Disposition: form-data; name=\"f1\"; filename=\"a.bin\"\r\n"
                                           "\r\n"
                                           "aaa\r\n"
                                           "--{}\r\n"
                                           "Content-Disposition: form-data; name=\"f2\"; filename=\"b.bin\"\r\n"
                                           "\r\n"
                                           "bbb\r\n"
                                           "--{}--\r\n",
                                           boundary,
                                           boundary,
                                           boundary);

            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::content_type,
                     std::format("multipart/form-data; boundary={}", boundary));
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/upload-multi", body, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });

    std::filesystem::remove_all(upload_dir);
}

TEST_CASE("Multipart randomized round-trip", "[http-methods]")
{
    auto upload_dir = std::filesystem::temp_directory_path() / "httplib_uploads_fuzz";
    std::filesystem::create_directories(upload_dir);

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> nfields_dist(1, 10);
    std::uniform_int_distribution<int> content_len(0, 4096);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    for (int round = 0; round < 20; ++round)
    {
        int nfields = nfields_dist(rng);
        std::vector<std::string> names;
        std::vector<std::string> contents;
        std::vector<std::string> filenames;

        std::string boundary = std::format("----FuzzBoundary{:04d}", round);
        std::string body;

        for (int i = 0; i < nfields; ++i)
        {
            auto name = std::format("field{}", i);
            bool is_file = (i % 3 == 0);
            auto filename = is_file ? std::format("file{}.bin", i) : "";
            int len = content_len(rng);
            std::string content;
            content.reserve(len);
            for (int j = 0; j < len; ++j)
            {
                content.push_back(static_cast<char>(byte_dist(rng)));
            }

            names.push_back(name);
            contents.push_back(content);
            filenames.push_back(filename);

            body += std::format("--{}\r\n", boundary);
            body += std::format("Content-Disposition: form-data; name=\"{}\"", name);
            if (is_file)
            {
                body += std::format("; filename=\"{}\"", filename);
            }
            body += "\r\n\r\n";
            body += content;
            body += "\r\n";
        }
        body += std::format("--{}--\r\n", boundary);

        run(
            [&](auto& server)
            {
                server.set_upload_dir(upload_dir);
                server.router().template set_http_handler<http::verb::post>(
                    "/fuzz",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    {
                        auto const& fd = req.body().template as<body::form_data_body>();
                        REQUIRE(fd.fields.size() == static_cast<size_t>(nfields));
                        for (int i = 0; i < nfields; ++i)
                        {
                            REQUIRE(fd.fields[i].name == names[i]);
                            if (filenames[i].empty())
                            {
                                REQUIRE(fd.fields[i].content == contents[i]);
                            }
                            else
                            {
                                REQUIRE(fd.fields[i].filename == filenames[i]);
                                REQUIRE(fd.fields[i].file_path.has_value());
                                std::ifstream f(*fd.fields[i].file_path,
                                                std::ios::binary);
                                std::string disk_content(
                                    (std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
                                REQUIRE(disk_content == contents[i]);
                            }
                        }
                        resp.set_string_content("ok"sv, "text/plain"sv);
                    });
            },
            [&](auto& client) -> net::awaitable<void>
            {
                auto hdrs = httplib::http::fields();
                hdrs.set(http::field::content_type,
                         std::format("multipart/form-data; boundary={}", boundary));
                auto resp = UNWRAP(co_await client.async_send_request(
                    http::verb::post, "/fuzz", body, hdrs));
                REQUIRE(resp.result() == http::status::ok);
                co_return;
            });

        for (auto& f : std::filesystem::directory_iterator(upload_dir))
        {
            std::filesystem::remove(f);
        }
    }

    std::filesystem::remove_all(upload_dir);
}

TEST_CASE("Multipart upload rejects path traversal via parent dir", "[http-methods]")
{
    auto upload_dir = std::filesystem::temp_directory_path() / "httplib_uploads_pt";
    std::filesystem::create_directories(upload_dir);

    run(
        [&](auto& server)
        {
            server.set_upload_dir(upload_dir);
            server.router().template set_http_handler<http::verb::post>(
                "/upload-pt",
                [&upload_dir](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& fd = req.body().template as<body::form_data_body>();
                    REQUIRE(fd.fields.size() == 1);
                    REQUIRE(fd.fields[0].filename == "../../../etc/passwd");
                    REQUIRE(fd.fields[0].file_path.has_value());
                    auto const& fp = fd.fields[0].file_path.value();
                    auto canonical_fp = std::filesystem::weakly_canonical(fp);
                    auto canonical_dir = std::filesystem::weakly_canonical(upload_dir);
                    REQUIRE(canonical_fp.string().find(canonical_dir.string()) == 0);
                    REQUIRE(fp.filename() == "passwd");
                    resp.set_string_content("ok"sv, "text/plain"sv);
                });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            std::string boundary = "----PathTraversal";
            std::string body = std::format("--{}\r\n"
                                           "Content-Disposition: form-data; name=\"file\"; "
                                           "filename=\"../../../etc/passwd\"\r\n"
                                           "\r\n"
                                           "evil-content\r\n"
                                           "--{}--\r\n",
                                           boundary,
                                           boundary);

            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::content_type,
                     std::format("multipart/form-data; boundary={}", boundary));
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/upload-pt", body, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });

    std::filesystem::remove_all(upload_dir);
}

TEST_CASE("Multipart upload strips absolute path filename to basename", "[http-methods]")
{
    auto upload_dir = std::filesystem::temp_directory_path() / "httplib_uploads_abs";
    std::filesystem::create_directories(upload_dir);

    run(
        [&](auto& server)
        {
            server.set_upload_dir(upload_dir);
            server.router().template set_http_handler<http::verb::post>(
                "/upload-abs",
                [&upload_dir](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& fd = req.body().template as<body::form_data_body>();
                    REQUIRE(fd.fields.size() == 1);
                    REQUIRE(fd.fields[0].file_path.has_value());
                    auto const& fp = fd.fields[0].file_path.value();
                    REQUIRE(fp.filename() == "evil.dll");
                    auto canonical_fp = std::filesystem::weakly_canonical(fp);
                    auto canonical_dir = std::filesystem::weakly_canonical(upload_dir);
                    REQUIRE(canonical_fp.string().find(canonical_dir.string()) == 0);
                    resp.set_string_content("ok"sv, "text/plain"sv);
                });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            std::string boundary = "----AbsPath";
            std::string body = std::format("--{}\r\n"
                                           "Content-Disposition: form-data; name=\"file\"; "
                                           "filename=\"C:\\\\Windows\\\\System32\\\\evil.dll\"\r\n"
                                           "\r\n"
                                           "payload\r\n"
                                           "--{}--\r\n",
                                           boundary,
                                           boundary);

            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::content_type,
                     std::format("multipart/form-data; boundary={}", boundary));
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/upload-abs", body, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });

    std::filesystem::remove_all(upload_dir);
}

TEST_CASE("Multipart upload basename-only safe filename", "[http-methods]")
{
    auto upload_dir = std::filesystem::temp_directory_path() / "httplib_uploads_safe";
    std::filesystem::create_directories(upload_dir);

    run(
        [&](auto& server)
        {
            server.set_upload_dir(upload_dir);
            server.router().template set_http_handler<http::verb::post>(
                "/upload-safe",
                [&upload_dir](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& fd = req.body().template as<body::form_data_body>();
                    REQUIRE(fd.fields.size() == 1);
                    REQUIRE(fd.fields[0].filename == "a/b/c/legit.txt");
                    REQUIRE(fd.fields[0].file_path.has_value());
                    auto const& fp = fd.fields[0].file_path.value();
                    REQUIRE(fp.filename() == "legit.txt");
                    REQUIRE(fp.parent_path() == upload_dir);
                    resp.set_string_content("ok"sv, "text/plain"sv);
                });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            std::string boundary = "----SafeName";
            std::string body = std::format("--{}\r\n"
                                           "Content-Disposition: form-data; name=\"file\"; "
                                           "filename=\"a/b/c/legit.txt\"\r\n"
                                           "\r\n"
                                           "safe-content\r\n"
                                           "--{}--\r\n",
                                           boundary,
                                           boundary);

            auto hdrs = httplib::http::fields();
            hdrs.set(http::field::content_type,
                     std::format("multipart/form-data; boundary={}", boundary));
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/upload-safe", body, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });

    std::filesystem::remove_all(upload_dir);
}

TEST_CASE("Server: handler throws exception returns 500", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/throw-std",
                [](httplib::server::request&, httplib::server::response&)
                { throw std::runtime_error("deliberate crash"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/throw-std"));
            REQUIRE(resp.result() == http::status::internal_server_error);
            co_return;
        });
}

TEST_CASE("Server: handler throws unknown exception returns 500", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/throw-unknown",
                [](httplib::server::request&, httplib::server::response&) { throw 42; });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/throw-unknown"));
            REQUIRE(resp.result() == http::status::internal_server_error);
            co_return;
        });
}

TEST_CASE("Server: read timeout", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.set_read_timeout(std::chrono::seconds(1));
            server.router().template set_http_handler<http::verb::post>(
                "/read-timeout",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.body().template as<body::string_body>() == "ok");
                    set_text(resp, "received"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/read-timeout", "ok"sv));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });
}

TEST_CASE("Server: write timeout", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.set_write_timeout(std::chrono::seconds(5));
            server.router().template set_http_handler<http::verb::get>(
                "/write-timeout",
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "ok"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/write-timeout"));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });
}

TEST_CASE("Body: empty string body in response", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/empty-str",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content(""sv, "text/plain"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/empty-str"));
            REQUIRE(resp.result() == http::status::ok);
            if (resp.body().template is_body_type<body::string_body>())
            {
                REQUIRE(resp.body().template as<body::string_body>().empty());
            }
            co_return;
        });
}

TEST_CASE("Body: empty JSON object", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/empty-json",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto& j = req.body().template as<body::json_body>();
                    REQUIRE(j.is_object());
                    REQUIRE(j.as_object().empty());
                    set_text(resp, "empty-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto hdrs = headers({
                { http::field::content_type, "application/json" }
            });
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/empty-json", "{}"sv, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "empty-ok");
            co_return;
        });
}

TEST_CASE("Body: JSON array as root", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/json-array",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto& j = req.body().template as<body::json_body>();
                    REQUIRE(j.is_array());
                    REQUIRE(j.as_array().size() == 3);
                    set_text(resp, "array-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto hdrs = headers({
                { http::field::content_type, "application/json" }
            });
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/json-array", "[1,2,3]"sv, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "array-ok");
            co_return;
        });
}

TEST_CASE("Body: large JSON body", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/large-json",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto& j = req.body().template as<body::json_body>();
                    REQUIRE(j.is_object());
                    REQUIRE(j.as_object().size() == 3);
                    set_text(resp, "large-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::string big_val(33000, 'x');
            auto body = std::format("{{\"a\":\"{}\",\"b\":1,\"c\":true}}", big_val);
            auto hdrs = headers({
                { http::field::content_type, "application/json" }
            });
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/large-json", body, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "large-ok");
            co_return;
        });
}

TEST_CASE("Body: urlencoded with special characters", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/url-special",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& params
                        = req.body().template as<body::query_params_body>();
                    auto it = params.params().find("msg");
                    REQUIRE(it != params.params().end());
                    REQUIRE(it->second == "hello world");
                    set_text(resp, "decoded-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto hdrs = headers({
                { http::field::content_type, "application/x-www-form-urlencoded" }
            });
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/url-special", "msg=hello%20world"sv, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "decoded-ok");
            co_return;
        });
}

TEST_CASE("Body: multipart form with empty field", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/multipart-empty",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto const& fd = req.body().template as<body::form_data_body>();
                    REQUIRE(fd.fields.size() == 1);
                    REQUIRE(fd.fields[0].name == "empty-field");
                    REQUIRE(fd.fields[0].content.empty());
                    REQUIRE(!fd.fields[0].is_file());
                    set_text(resp, "empty-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::string boundary = "----BoundaryEmpty";
            std::string body = std::format("--{}\r\n"
                                           "Content-Disposition: form-data; name=\"empty-field\"\r\n"
                                           "\r\n"
                                           "\r\n"
                                           "--{}--\r\n",
                                           boundary,
                                           boundary);

            auto hdrs = headers({
                { http::field::content_type,
                  std::format("multipart/form-data; boundary={}", boundary) }
            });
            auto resp = UNWRAP(co_await client.async_send_request(
                http::verb::post, "/multipart-empty", body, hdrs));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "empty-ok");
            co_return;
        });
}

TEST_CASE("Body: response JSON with non-object root", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/json-resp",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set_json_content({
                        { "ok", true }
                    });
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/json-resp"));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });
}

TEST_CASE("Body: empty_body on empty POST request", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/empty-post",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE(req.body().template is_body_type<body::empty_body>());
                    set_text(resp, "empty-ok"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_post("/empty-post"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "empty-ok");
            co_return;
        });
}

TEST_CASE("JSON randomized round-trip", "[http-methods]")
{
    std::mt19937 rng(123);
    std::uniform_int_distribution<int> nkeys_dist(1, 12);
    std::uniform_int_distribution<int> strlen_dist(0, 256);
    std::uniform_int_distribution<int> byte_dist(32, 126);
    std::uniform_int_distribution<int> type_dist(0, 3);

    for (int round = 0; round < 15; ++round)
    {
        int nkeys = nkeys_dist(rng);
        boost::json::object sent;
        for (int i = 0; i < nkeys; ++i)
        {
            auto key = std::format("key{:02d}", i);
            switch (type_dist(rng))
            {
                case 0:
                {
                    int len = strlen_dist(rng);
                    std::string val;
                    val.reserve(len);
                    for (int j = 0; j < len; ++j)
                    {
                        val.push_back(static_cast<char>(byte_dist(rng)));
                    }
                    sent[key] = val;
                    break;
                }
                case 1:
                    sent[key] = static_cast<int64_t>(byte_dist(rng));
                    break;
                case 2:
                    sent[key] = (type_dist(rng) % 2 == 0);
                    break;
                case 3:
                    sent[key] = nullptr;
                    break;
            }
        }

        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::post>(
                    "/json-fuzz",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    {
                        auto& j = req.body().template as<body::json_body>();
                        REQUIRE(j.is_object());
                        REQUIRE(j.as_object() == sent);
                        resp.set_json_content({
                            { "ok", true }
                        });
                    });
            },
            [&](auto& client) -> net::awaitable<void>
            {
                auto hdrs = httplib::http::fields();
                hdrs.set(http::field::content_type, "application/json");
                auto body = boost::json::serialize(sent);
                auto resp = UNWRAP(co_await client.async_send_request(
                    http::verb::post, "/json-fuzz", body, hdrs));
                REQUIRE(resp.result() == http::status::ok);
                co_return;
            });
    }
}

TEST_CASE("Query params randomized round-trip", "[http-methods]")
{
    std::mt19937 rng(456);
    std::uniform_int_distribution<int> npairs_dist(1, 10);
    std::uniform_int_distribution<int> val_len_dist(0, 32);
    std::uniform_int_distribution<int> char_dist(97, 122);

    for (int round = 0; round < 15; ++round)
    {
        httplib::html::query_params sent;
        std::vector<std::pair<std::string, std::string>> expected;
        int npairs = npairs_dist(rng);
        for (int i = 0; i < npairs; ++i)
        {
            auto key = std::format("p{:02d}", i);
            int len = val_len_dist(rng);
            std::string val;
            val.reserve(len);
            for (int j = 0; j < len; ++j)
            {
                val.push_back(static_cast<char>(char_dist(rng)));
            }
            sent.add(key, val);
            expected.emplace_back(key, val);
        }

        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::post>(
                    "/query-fuzz",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    {
                        auto const& pq = req.body().template as<body::query_params_body>();
                        auto const& map = pq.params();
                        REQUIRE(map.size() == expected.size());
                        resp.set_string_content("ok"sv, "text/plain"sv);
                    });
            },
            [&](auto& client) -> net::awaitable<void>
            {
                auto hdrs = httplib::http::fields();
                hdrs.set(http::field::content_type, "application/x-www-form-urlencoded");
                auto resp = UNWRAP(co_await client.async_send_request(
                    http::verb::post, "/query-fuzz", sent.encoded(), hdrs));
                REQUIRE(resp.result() == http::status::ok);
                co_return;
            });
    }
}

TEST_CASE("Server: header limit rejects oversized headers", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.set_header_limit(128);
            server.router().template set_http_handler<http::verb::get>(
                "/hdr-limit",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto hdrs = httplib::http::fields();
            std::string big_value(200, 'A');
            hdrs.set("X-Big-Header", big_value);
            auto resp = co_await client.async_send_request(http::verb::get, "/hdr-limit", hdrs);
            REQUIRE_FALSE(resp.has_value());
            co_return;
        });
}

TEST_CASE("Server: body limit rejects oversized body", "[http-methods]")
{
    run(
        [](auto& server)
        {
            server.set_body_limit(16);
            server.router().template set_http_handler<http::verb::post>(
                "/body-limit",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("ok"sv, "text/plain"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            std::string big_body(100, 'X');
            auto resp = co_await client.async_send_request(
                http::verb::post, "/body-limit", big_body);
            REQUIRE_FALSE(resp.has_value());
            co_return;
        });
}
