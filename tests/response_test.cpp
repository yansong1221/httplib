#include "common.hpp"
#include "httplib/body/file_body.hpp"
#include "httplib/body/form_data_body.hpp"
#include "httplib/body/json_body.hpp"
#include "httplib/body/string_body.hpp"
#include "httplib/client/lazy_request.hpp"
#include "httplib/server/chunk_writer.hpp"
#include "httplib/server/mount_point_entry.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <array>
#include <boost/beast/core/flat_buffer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace body = httplib::body;
namespace http = httplib::http;
namespace net = httplib::net;
using test_common::run;
using test_common::setup_logger;

namespace
{

    void
    set_text(httplib::server::response& resp, std::string_view body, http::status status = http::status::ok)
    {
        resp.set_string_content(body, "text/plain"sv, status);
    }

} // namespace

TEST_CASE("Response: set_empty_content", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/empty",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_empty_content(http::status::no_content); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/empty"));
            REQUIRE(resp.result() == http::status::no_content);
            co_return;
        });
}

TEST_CASE("Response: set_error_content", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/error",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_error_content(http::status::internal_server_error); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/error"));
            REQUIRE(resp.result() == http::status::internal_server_error);
            REQUIRE_FALSE(resp.body().template as<body::string_body>().empty());
            co_return;
        });
}

TEST_CASE("Response: set_redirect", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/old",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect("/new", http::status::moved_permanently); });
            server.router().template set_http_handler<http::verb::get>(
                "/new",
                [](httplib::server::request&, httplib::server::response& resp)
                { set_text(resp, "new-location"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/old"));
            REQUIRE(resp.result() == http::status::moved_permanently);
            REQUIRE(resp[http::field::location] == "/new");
            co_return;
        });
}

TEST_CASE("Response: set_chunked_write_handler with multiple chunks", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/stream",
                [](httplib::server::request&,
                   httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.get_chunk_writer();
                    http::fields headers;
                    headers.set(http::field::content_type, "text/plain");
                    co_await cw->write_header(http::status::ok, headers, false);
                    constexpr std::string_view chunks[] = { "A", "B", "C" };
                    for (int i = 0; i < 3; ++i)
                    {
                        co_await cw->write_body(net::buffer(chunks[i]), i < 2);
                    }
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto writer = client.create_lazy_request();
            std::string streamed;
            co_await writer->write_header(http::verb::get, "/stream", {});
            co_await writer->write_body(net::buffer("", 0), false);

            auto resp = UNWRAP(co_await writer->read_response());
            std::array<char, 4096> buf;
            while (true)
            {
                auto result = co_await resp.read_some(net::buffer(buf));
                if (result.has_error() || result.value() == 0)
                    break;
                streamed.append(buf.data(), result.value());
            }

            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(streamed == "ABC");
            co_return;
        });
}

TEST_CASE("Response: set_form_data_content", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/form-resp",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    std::vector<httplib::html::form_data::field> fields;
                    fields.push_back({ "name", "", "text/plain", "test-value" });
                    resp.set_form_data_content(std::move(fields));
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/form-resp"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE_FALSE(resp[http::field::content_type].empty());
            REQUIRE(resp[http::field::content_type].starts_with("multipart/form-data"));
            co_return;
        });
}

TEST_CASE("Response: set_form_data_content with file_path", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_form_file.bin";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "file content from disk";
    }

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/form-file",
                [&](httplib::server::request&, httplib::server::response& resp)
                {
                    std::vector<httplib::html::form_data::field> fields;
                    auto& fld = fields.emplace_back();
                    fld.name = "file";
                    fld.filename = "test.bin";
                    fld.content_type = "application/octet-stream";
                    fld.file_path = tmp_path;
                    resp.set_form_data_content(std::move(fields));
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/form-file"));
            REQUIRE(resp.result() == http::status::ok);
            auto& fd = resp.body().template as<body::form_data_body>();
            REQUIRE(fd.fields.size() == 1);
            REQUIRE(fd.fields[0].name == "file");
            REQUIRE(fd.fields[0].filename == "test.bin");
            REQUIRE(fd.fields[0].content == "file content from disk");
            co_return;
        });

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: set_file_content serves a file", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_file.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "hello from test file\n";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/file",
                    [&](httplib::server::request&, httplib::server::response& resp)
                    { resp.set_file_content(tmp_path); });
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_get("/file"));
                REQUIRE(resp.result() == http::status::ok);
                REQUIRE(resp.body().template as<body::string_body>() == "hello from test file\n");
                co_return;
            });
    }

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: set_file_content with Range request", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_range.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "0123456789";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/file-range",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    { resp.set_file_content(tmp_path, req.base()); });
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto range_headers = httplib::http::fields();
                range_headers.set(http::field::range, "bytes=0-4");

                auto resp = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::get, "/file-range", range_headers)));
                REQUIRE(resp.result() == http::status::partial_content);
                REQUIRE(resp.body().template as<body::string_body>() == "01234");
                co_return;
            });
    }

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Range request open-ended (bytes=N-)", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_range_open.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "0123456789";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/file-range-open",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    { resp.set_file_content(tmp_path, req.base()); });
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto range_headers = httplib::http::fields();
                range_headers.set(http::field::range, "bytes=7-");

                auto resp = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::get, "/file-range-open", range_headers)));
                REQUIRE(resp.result() == http::status::partial_content);
                REQUIRE(resp.body().template as<body::string_body>() == "789");
                co_return;
            });
    }

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Range request suffix (bytes=-N)", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_range_suffix.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "0123456789";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/file-range-suffix",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    { resp.set_file_content(tmp_path, req.base()); });
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto range_headers = httplib::http::fields();
                range_headers.set(http::field::range, "bytes=-4");

                auto resp = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::get, "/file-range-suffix", range_headers)));
                REQUIRE(resp.result() == http::status::partial_content);
                REQUIRE(resp.body().template as<body::string_body>() == "6789");
                co_return;
            });
    }

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Range request Content-Range header", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_cr.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "abcdefghij";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/file-cr",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    { resp.set_file_content(tmp_path, req.base()); });
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto range_headers = httplib::http::fields();
                range_headers.set(http::field::range, "bytes=2-5");

                auto resp = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::get, "/file-cr", range_headers)));
                REQUIRE(resp.result() == http::status::partial_content);
                REQUIRE(resp.body().template as<body::string_body>() == "cdef");
                REQUIRE(resp.base().find(http::field::content_range) != resp.base().end());
                REQUIRE_FALSE(std::string(resp[http::field::content_range]).empty());
                co_return;
            });
    }

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Range request out of bounds returns 416", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_range_oob.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "01234";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/file-range-oob",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    { resp.set_file_content(tmp_path, req.base()); });
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto range_headers = httplib::http::fields();
                range_headers.set(http::field::range, "bytes=10-20");

                auto resp = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::get, "/file-range-oob", range_headers)));
                REQUIRE(resp.result() == http::status::range_not_satisfiable);
                REQUIRE(resp.base().find(http::field::content_range) != resp.base().end());
                co_return;
            });
    }

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: If-None-Match returns 304 for matching ETag", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_etag.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "etag-content";
    }

    std::string etag;
    {
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/file-etag",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    { resp.set_file_content(tmp_path, req.base()); });
            },
            [&](auto& client) -> net::awaitable<void>
            {
                auto resp1 = UNWRAP(co_await client.async_get("/file-etag"));
                REQUIRE(resp1.result() == http::status::ok);
                REQUIRE(resp1.base().find(http::field::etag) != resp1.base().end());
                etag = std::string(resp1[http::field::etag]);

                auto hdrs = httplib::http::fields();
                hdrs.set(http::field::if_none_match, etag);
                auto resp2 = UNWRAP(
                    co_await client.async_send_request(httplib::client::request(http::verb::get, "/file-etag", hdrs)));
                REQUIRE(resp2.result() == http::status::not_modified);
                co_return;
            });
    }

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: If-Modified-Since returns 304 for unmodified", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_ims.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "ims-content";
    }

    std::string last_mod;
    {
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/file-ims",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    { resp.set_file_content(tmp_path, req.base()); });
            },
            [&](auto& client) -> net::awaitable<void>
            {
                auto resp1 = UNWRAP(co_await client.async_get("/file-ims"));
                REQUIRE(resp1.result() == http::status::ok);
                REQUIRE(resp1.base().find(http::field::last_modified) != resp1.base().end());
                last_mod = std::string(resp1[http::field::last_modified]);

                auto hdrs = httplib::http::fields();
                hdrs.set(http::field::if_modified_since, last_mod);
                auto resp2 = UNWRAP(
                    co_await client.async_send_request(httplib::client::request(http::verb::get, "/file-ims", hdrs)));
                REQUIRE(resp2.result() == http::status::not_modified);
                co_return;
            });
    }

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Accept-Ranges header present on full response", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_ar.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "test";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/file-ar",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    { resp.set_file_content(tmp_path, req.base()); });
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_get("/file-ar"));
                REQUIRE(resp.result() == http::status::ok);
                REQUIRE(resp.base().find(http::field::accept_ranges) != resp.base().end());
                REQUIRE(std::string(resp[http::field::accept_ranges]) == "bytes");
                co_return;
            });
    }

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Multi-range request returns multipart/byteranges", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_multi_range.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "0123456789";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().template set_http_handler<http::verb::get>(
                    "/file-multi-range",
                    [&](httplib::server::request& req, httplib::server::response& resp)
                    { resp.set_file_content(tmp_path, req.base()); });
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto range_headers = httplib::http::fields();
                range_headers.set(http::field::range, "bytes=0-2,5-7");

                auto resp = UNWRAP(co_await client.async_send_request(httplib::client::request(http::verb::get, "/file-multi-range", range_headers)));
                REQUIRE(resp.result() == http::status::partial_content);
                auto ct = std::string(resp[http::field::content_type]);
                REQUIRE(ct.starts_with("multipart/byteranges"));
                REQUIRE_FALSE(ct.find("boundary=") == std::string::npos);
                co_return;
            });
    }

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: custom response headers", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/custom-headers",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set("X-Custom-One", "value1");
                    resp.set("X-Custom-Two", "value2");
                    set_text(resp, "ok");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/custom-headers"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp["X-Custom-One"] == "value1");
            REQUIRE(resp["X-Custom-Two"] == "value2");
            co_return;
        });
}

TEST_CASE("Response: redirect 302", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/redirect-302",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect("/target", http::status::found); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/redirect-302"));
            REQUIRE(resp.result() == http::status::found);
            REQUIRE(resp[http::field::location] == "/target");
            co_return;
        });
}

TEST_CASE("Response: redirect 303", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/redirect-303",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect("/target", http::status::see_other); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/redirect-303"));
            REQUIRE(resp.result() == http::status::see_other);
            REQUIRE(resp[http::field::location] == "/target");
            co_return;
        });
}

TEST_CASE("Response: redirect 307", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/redirect-307",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_redirect("/target", http::status::temporary_redirect); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/redirect-307"));
            REQUIRE(resp.result() == http::status::temporary_redirect);
            REQUIRE(resp[http::field::location] == "/target");
            co_return;
        });
}

TEST_CASE("Response: keep-alive close response", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/close-conn",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    resp.set(http::field::connection, "close");
                    resp.set_string_content("closing"sv, "text/plain"sv);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/close-conn"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp.body().template as<body::string_body>() == "closing");
            REQUIRE(resp[http::field::connection] == "close");
            co_return;
        });
}

TEST_CASE("Static mount: serves a file", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static";
    auto filepath = tmp_dir / "test.txt";
    std::filesystem::create_directories(tmp_dir);
    {
        std::ofstream f(filepath);
        f << "static content";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().set_static_mount_point("/static", tmp_dir);
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_get("/static/test.txt"));
                REQUIRE(resp.result() == http::status::ok);
                REQUIRE(resp.body().template as<body::string_body>() == "static content");
                co_return;
            });
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: returns 404 for non-existent file", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_404";
    std::filesystem::create_directories(tmp_dir);

    run(
        [&](auto& server)
        {
            server.router().set_static_mount_point("/files", tmp_dir);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/files/nope.txt"));
            REQUIRE(resp.result() == http::status::not_found);
            co_return;
        });

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: blocks path traversal", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_pt";
    std::filesystem::create_directories(tmp_dir);

    run(
        [&](auto& server)
        {
            server.router().set_static_mount_point("/files", tmp_dir);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/files/../../../etc/passwd"));
            REQUIRE(resp.result() == http::status::bad_request);
            co_return;
        });

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: default document index.html", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_dd";
    std::filesystem::create_directories(tmp_dir);
    {
        std::ofstream f(tmp_dir / "index.html");
        f << "<h1>hello</h1>";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().set_static_mount_point("/", tmp_dir);
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_get("/"));
                REQUIRE(resp.result() == http::status::ok);
                REQUIRE(resp.body().template as<body::string_body>() == "<h1>hello</h1>");
                co_return;
            });
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: directory listing via subpath", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_list";
    auto sub_dir = tmp_dir / "data";
    std::filesystem::create_directories(sub_dir);
    {
        std::ofstream f(sub_dir / "a.txt");
        f << "a";
    }

    {
        run(
            [&](auto& server)
            {
                auto entry = httplib::server::mount_point_entry("/dir", tmp_dir);
                entry.set_enabled_directory(true);
                entry.set_directory_format(
                    httplib::server::mount_point_entry::dir_format_type::json);
                server.router().set_static_mount_point(std::move(entry));
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_get("/dir/data/"));
                REQUIRE(resp.result() == http::status::ok);
                co_return;
            });
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: non-existent file returns 404 via mount", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_no";
    std::filesystem::create_directories(tmp_dir);

    run(
        [&](auto& server)
        {
            server.router().set_static_mount_point("/pub", tmp_dir);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/pub/nope.txt"));
            REQUIRE(resp.result() == http::status::not_found);
            co_return;
        });

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: file in subdirectory", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_sub";
    auto sub_dir = tmp_dir / "sub";
    std::filesystem::create_directories(sub_dir);
    {
        std::ofstream f(sub_dir / "deep.txt");
        f << "nested";
    }

    {
        run(
            [&](auto& server)
            {
                server.router().set_static_mount_point("/pub", tmp_dir);
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_get("/pub/sub/deep.txt"));
                REQUIRE(resp.result() == http::status::ok);
                REQUIRE(resp.body().template as<body::string_body>() == "nested");
                co_return;
            });
    }

    std::filesystem::remove_all(tmp_dir);
}
