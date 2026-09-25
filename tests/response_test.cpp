#include "common.hpp"
#include "compress/compressor.hpp"
#include "httplib/client/lazy_request.hpp"
#include "httplib/server/mount_point_entry.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/stream_writer.hpp"
#include <array>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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
            REQUIRE_FALSE(resp.as_string().empty());
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
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "new-location"); });
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
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.create_stream_writer();
                    http::fields headers;
                    headers.set(http::field::content_type, "text/plain");
                    co_await cw->write_header(http::status::ok, headers, httplib::server::stream_writer::mode::chunked);
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
            co_await writer->write_header(http::verb::get, "/stream", {}, httplib::client::lazy_request::mode::relay);
            co_await writer->write_body(net::buffer("", 0), false);

            auto resp = UNWRAP(co_await writer->read_response_lazy());
            std::array<char, 4096> buf;
            boost::system::error_code ec;
            while (true)
            {
                auto result = co_await resp.read_some_raw(net::buffer(buf), ec);
                if (ec || result == 0)
                {
                    break;
                }
                streamed.append(buf.data(), result);
            }

            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(streamed == "ABC");
            co_return;
        });
}

#ifdef HTTPLIB_ENABLED_COMPRESS
TEST_CASE("Response: stream_writer gzip compression", "[response][compression]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/stream-gzip",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.create_stream_writer();
                    http::fields headers;
                    headers.set(http::field::content_type, "text/plain");
                    headers.set(http::field::content_encoding, "gzip");
                    co_await cw->write_header(http::status::ok, headers, httplib::server::stream_writer::mode::chunked);
                    constexpr std::string_view chunks[] = { "A", "B", "C" };
                    for (int i = 0; i < 3; ++i)
                    {
                        co_await cw->write_body(net::buffer(chunks[i]), i < 2);
                    }
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/stream-gzip"));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp[http::field::content_encoding] == "gzip");
            REQUIRE(resp.as_string() == "ABC");
            co_return;
        });
}
#endif

TEST_CASE("Response: set_form_data_content", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/form-resp",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    std::vector<httplib::form_data::field> fields;
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
                    std::vector<httplib::form_data::field> fields;
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
            auto const& fd = resp.as_form_data();
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
                REQUIRE(resp.as_string() == "hello from test file\n");
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

                httplib::client::request req(http::verb::get, "/file-range", range_headers);
                auto resp = UNWRAP(co_await client.async_send_request(req));
                REQUIRE(resp.result() == http::status::partial_content);
                REQUIRE(resp.as_string() == "01234");
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

                httplib::client::request req(http::verb::get, "/file-range-open", range_headers);
                auto resp = UNWRAP(co_await client.async_send_request(req));
                REQUIRE(resp.result() == http::status::partial_content);
                REQUIRE(resp.as_string() == "789");
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

                httplib::client::request req(http::verb::get, "/file-range-suffix", range_headers);
                auto resp = UNWRAP(co_await client.async_send_request(req));
                REQUIRE(resp.result() == http::status::partial_content);
                REQUIRE(resp.as_string() == "6789");
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

                httplib::client::request req(http::verb::get, "/file-cr", range_headers);
                auto resp = UNWRAP(co_await client.async_send_request(req));
                REQUIRE(resp.result() == http::status::partial_content);
                REQUIRE(resp.as_string() == "cdef");
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

                httplib::client::request req(http::verb::get, "/file-range-oob", range_headers);
                auto resp = UNWRAP(co_await client.async_send_request(req));
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
                httplib::client::request req(http::verb::get, "/file-etag", hdrs);
                auto resp2 = UNWRAP(co_await client.async_send_request(req));
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
                httplib::client::request req(http::verb::get, "/file-ims", hdrs);
                auto resp2 = UNWRAP(co_await client.async_send_request(req));
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

                httplib::client::request req(http::verb::get, "/file-multi-range", range_headers);
                auto resp = UNWRAP(co_await client.async_send_request(req));
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
            REQUIRE(resp.as_string() == "closing");
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
        run([&](auto& server) { server.router().set_static_mount_point("/static", tmp_dir); },
            [](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_get("/static/test.txt"));
                REQUIRE(resp.result() == http::status::ok);
                REQUIRE(resp.as_string() == "static content");
                co_return;
            });
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: returns 404 for non-existent file", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_404";
    std::filesystem::create_directories(tmp_dir);

    run([&](auto& server) { server.router().set_static_mount_point("/files", tmp_dir); },
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

    run([&](auto& server) { server.router().set_static_mount_point("/files", tmp_dir); },
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
        run([&](auto& server) { server.router().set_static_mount_point("/", tmp_dir); },
            [](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_get("/"));
                REQUIRE(resp.result() == http::status::ok);
                REQUIRE(resp.as_string() == "<h1>hello</h1>");
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
                entry.set_directory_format(httplib::server::mount_point_entry::dir_format_type::json);
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

TEST_CASE("Static mount: html directory listing escapes target and file names", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_html";
    std::filesystem::create_directories(tmp_dir);
    {
        std::ofstream f(tmp_dir / "a&b.txt");
        f << "x";
    }

    {
        run(
            [&](auto& server)
            {
                // mount point 故意包含 '<' '>'：目录列表的 target 来自请求路径，
                // 必须被 HTML 转义，不能原样进入 <title>/<h1>。
                auto entry = httplib::server::mount_point_entry("/<x>", tmp_dir);
                entry.set_enabled_directory(true);
                entry.set_directory_format(httplib::server::mount_point_entry::dir_format_type::html);
                server.router().set_static_mount_point(std::move(entry));
            },
            [](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_get("/<x>/"));
                REQUIRE(resp.result() == http::status::ok);
                REQUIRE(resp[http::field::content_type] == "text/html; charset=utf-8");

                auto body = resp.as_string();

                // target 中的 '<' '>' 必须 HTML 转义。
                REQUIRE(body.find("&lt;x&gt;") != std::string::npos);
                REQUIRE(body.find("<x>") == std::string::npos);

                // 文件名中的 '&' 必须 HTML 转义，href 必须 URL 编码。
                REQUIRE(body.find("a&amp;b.txt") != std::string::npos);
                REQUIRE(body.find("a%26b.txt") != std::string::npos);

                co_return;
            });
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: symlink escaping base dir is blocked", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_symlink";
    auto out_dir = std::filesystem::temp_directory_path() / "httplib_static_symlink_out";
    std::filesystem::create_directories(tmp_dir);
    std::filesystem::create_directories(out_dir);
    {
        std::ofstream f(out_dir / "secret.txt");
        f << "secret";
    }
    std::error_code ec;
    std::filesystem::create_symlink(out_dir / "secret.txt", tmp_dir / "link.txt", ec);
    if (ec)
    {
        SKIP("symlink creation not permitted");
    }

    run([&](auto& server) { server.router().set_static_mount_point("/files", tmp_dir); },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/files/link.txt"));
            REQUIRE(resp.result() != http::status::ok);
            co_return;
        });

    std::filesystem::remove_all(tmp_dir);
    std::filesystem::remove_all(out_dir);
}

TEST_CASE("Static mount: non-existent file returns 404 via mount", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_no";
    std::filesystem::create_directories(tmp_dir);

    run([&](auto& server) { server.router().set_static_mount_point("/pub", tmp_dir); },
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
        run([&](auto& server) { server.router().set_static_mount_point("/pub", tmp_dir); },
            [](auto& client) -> net::awaitable<void>
            {
                auto resp = UNWRAP(co_await client.async_get("/pub/sub/deep.txt"));
                REQUIRE(resp.result() == http::status::ok);
                REQUIRE(resp.as_string() == "nested");
                co_return;
            });
    }

    std::filesystem::remove_all(tmp_dir);
}

namespace
{
    constexpr std::string_view kGzipPayload
        = "the quick brown fox jumps over the lazy dog and keeps jumping over the lazy hedgehog while the "
          "streaming decompressor keeps up with the pace of the wire";
} // namespace

#ifdef HTTPLIB_ENABLED_COMPRESS
TEST_CASE("Response: read_some_decompressed decodes gzip body", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/gzip-stream",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, kGzipPayload); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::http::fields headers;
            headers.set(http::field::accept_encoding, "gzip");
            httplib::client::request req(http::verb::get, "/gzip-stream", headers);
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(resp[http::field::content_encoding] == "gzip");

            std::string decoded;
            std::array<char, 7> buf;
            boost::system::error_code ec;
            while (true)
            {
                auto result = co_await resp.read_some_decompressed(net::buffer(buf), ec);
                if (ec || result == 0)
                {
                    break;
                }
                decoded.append(buf.data(), result);
            }
            REQUIRE(decoded == kGzipPayload);
            co_return;
        });
}

TEST_CASE("Response: read_some_decompressed with single large buffer", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/gzip-big",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, kGzipPayload); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::http::fields headers;
            headers.set(http::field::accept_encoding, "gzip");
            httplib::client::request req(http::verb::get, "/gzip-big", headers);
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp[http::field::content_encoding] == "gzip");

            std::array<char, 4096> buf;
            boost::system::error_code ec1;
            auto r1 = co_await resp.read_some_decompressed(net::buffer(buf), ec1);
            REQUIRE_FALSE(ec1);
            REQUIRE(r1 == kGzipPayload.size());
            std::string decoded(buf.data(), r1);
            boost::system::error_code ec2;
            auto r2 = co_await resp.read_some_decompressed(net::buffer(buf), ec2);
            REQUIRE_FALSE(ec2);
            REQUIRE(r2 == 0);
            REQUIRE(decoded == kGzipPayload);
            co_return;
        });
}
#endif

TEST_CASE("Response: read_some_decompressed passes through identity body", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/plain",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, kGzipPayload); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::http::fields headers;
            headers.set(http::field::accept_encoding, "identity");
            httplib::client::request req(http::verb::get, "/plain", headers);
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == http::status::ok);
            REQUIRE_FALSE(resp[http::field::content_encoding] == "gzip");

            std::string decoded;
            std::array<char, 16> buf;
            boost::system::error_code ec;
            while (true)
            {
                auto result = co_await resp.read_some_decompressed(net::buffer(buf), ec);
                if (ec || result == 0)
                {
                    break;
                }
                decoded.append(buf.data(), result);
            }
            REQUIRE(decoded == kGzipPayload);
            co_return;
        });
}

TEST_CASE("Response: unsupported request content-encoding is dropped", "[response]")
{
    bool saw_encoding = false;
    std::string got_body;
    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/enc",
                [&](httplib::server::request& req, httplib::server::response& resp)
                {
                    saw_encoding = !req[http::field::content_encoding].empty();
                    got_body = req.as_string();
                    resp.set_string_content("ok"sv, "text/plain"sv);
                });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            httplib::http::fields headers;
            headers.set(http::field::content_encoding, "unsupported-encoding-xyz");
            auto req = httplib::client::request(http::verb::post, "/enc", headers);
            req.set_body(std::string("plain hello body"), "text/plain"sv);
            auto resp = UNWRAP(co_await client.async_send_request(req));
            REQUIRE(resp.result() == http::status::ok);
            co_return;
        });
    REQUIRE_FALSE(saw_encoding);
    REQUIRE(got_body == "plain hello body");
}

#ifdef HTTPLIB_ENABLED_COMPRESS
TEST_CASE("Response: is_body_done reflects decompressed pending overflow", "[response]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<http::verb::get>(
                "/gzip-done",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, kGzipPayload); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::http::fields headers;
            headers.set(http::field::accept_encoding, "gzip");
            httplib::client::request req(http::verb::get, "/gzip-done", headers);
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE_FALSE(resp.is_body_done());

            std::array<char, 5> buf;
            std::size_t total = 0;
            boost::system::error_code ec;
            for (;;)
            {
                auto r = co_await resp.read_some_decompressed(net::buffer(buf), ec);
                if (ec || r == 0)
                {
                    break;
                }
                total += r;
            }
            REQUIRE(total == kGzipPayload.size());
            REQUIRE(resp.is_body_done());
            co_return;
        });
}

TEST_CASE("Response: decompressed body limit rejects compression bomb", "[response]")
{
    std::string big_original(10240, 'X');
    bool handler_called = false;

    test_common::run(
        [&](auto& server)
        {
            server.router().template set_http_handler<http::verb::post>(
                "/bomb",
                [&](httplib::server::request&, httplib::server::response& resp)
                {
                    handler_called = true;
                    resp.set_string_content("ok"sv, "text/plain"sv);
                });
            server.set_body_limit(1024);
        },
        [&](auto& client) -> net::awaitable<void>
        {
            httplib::http::fields headers;
            headers.set(http::field::content_encoding, "gzip");
            auto req = httplib::client::request(http::verb::post, "/bomb", headers);
            req.set_body(big_original, "text/plain"sv);
            auto resp = co_await client.async_send_request(req);
            REQUIRE_FALSE(resp.has_value());
            REQUIRE_FALSE(handler_called);
            co_return;
        });
}

TEST_CASE("Response: malformed gzip request body errors cleanly, no exception", "[response]")
{
    std::string compressed;
    {
        auto& factory = httplib::compress::compressor_factory::instance();
        auto encoder = factory.create("gzip");
        REQUIRE(encoder);
        boost::system::error_code ec;
        encoder->init(httplib::compress::compressor::mode::encode, ec);
        REQUIRE_FALSE(ec);
        encoder->write(net::buffer(std::string(4096, 'X')), false, ec);
        REQUIRE_FALSE(ec);
        auto buf = encoder->buffer();
        compressed.assign(static_cast<char const*>(buf.data()), buf.size());
    }
    std::string truncated(compressed.data(), compressed.size() / 2);
    REQUIRE(!truncated.empty());
    REQUIRE(truncated.size() < compressed.size());

    bool handler_called = false;
    net::thread_pool pool { 2 };
    std::exception_ptr err;
    net::co_spawn(
        pool.get_executor(),
        [&]() -> net::awaitable<void>
        {
            httplib::server::http_server server(pool.get_executor());
            test_common::setup_logger(server);
            server.router().template set_http_handler<http::verb::post>(
                "/bomb",
                [&](httplib::server::request&, httplib::server::response& resp)
                {
                    handler_called = true;
                    resp.set_string_content("ok"sv, "text/plain"sv);
                });
            server.listen("127.0.0.1", 0);
            auto ep = server.local_endpoint();
            server.run();

            try
            {
                // 直接用原始 socket 发送截断的 gzip 请求体，绕过客户端 writer。
                net::ip::tcp::socket sock(pool.get_executor());
                boost::system::error_code ec;
                co_await sock.async_connect(ep, net::redirect_error(net::use_awaitable, ec));
                REQUIRE_FALSE(ec);

                std::string raw = "POST /bomb HTTP/1.1\r\n"
                                  "Host: 127.0.0.1\r\n"
                                  "Content-Encoding: gzip\r\n"
                                  "Content-Length: "
                                  + std::to_string(truncated.size()) + "\r\n\r\n" + truncated;
                co_await net::async_write(sock, net::buffer(raw), net::redirect_error(net::use_awaitable, ec));
                REQUIRE_FALSE(ec);

                // 服务端在解压失败后应直接断开连接，而不是抛异常回 500。
                std::array<char, 512> buf {};
                std::string received;
                boost::system::error_code re;
                while (!re)
                {
                    auto n
                        = co_await sock.async_read_some(net::buffer(buf), net::redirect_error(net::use_awaitable, re));
                    received.append(buf.data(), n);
                }
                sock.close();
                server.stop();

                REQUIRE_FALSE(handler_called);
                REQUIRE(received.empty());
            }
            catch (...)
            {
                server.stop();
                throw;
            }
        },
        [&](std::exception_ptr e) { err = e; });
    pool.join();
    if (err)
    {
        std::rethrow_exception(err);
    }
}
#endif
