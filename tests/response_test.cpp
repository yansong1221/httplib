#include "httplib/body/file_body.hpp"
#include "httplib/body/json_body.hpp"
#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/read_session.hpp"
#include "httplib/client/write_session.hpp"
#include "httplib/server/mount_point_entry.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <array>
#include <boost/beast/core/flat_buffer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

using namespace std::string_view_literals;
namespace beast = httplib::beast;
namespace http = httplib::http;
namespace net = httplib::net;

namespace
{

    struct test_scaffold
    {
        net::io_context ioc;
        httplib::server::http_server server;
        httplib::tcp::endpoint endpoint;
        std::thread thread;
        std::unique_ptr<httplib::client::http_client> client;

        test_scaffold() : server(ioc)
        {
            auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            server.set_logger(std::make_shared<spdlog::logger>("httplib.tests", null_sink));
        }

        ~test_scaffold()
        {
            server.stop().wait();
            ioc.stop();
            if (thread.joinable())
            {
                thread.join();
            }
        }

        void
        start()
        {
            server.listen("127.0.0.1", 0);
            endpoint = server.local_endpoint();
            server.run();
            thread = std::thread([this] { ioc.run(); });

            client = std::make_unique<httplib::client::http_client>(ioc.get_executor(),
                                                                    endpoint.address().to_string(),
                                                                    endpoint.port());
            client->set_timeout(std::chrono::seconds(5));
        }

        auto&
        router()
        {
            return server.router();
        }
    };

    std::string
    as_string(httplib::client::http_client::response const& resp)
    {
        return resp.body().as<httplib::body::string_body>();
    }

    void
    set_text(httplib::server::response& resp, std::string_view body, http::status status = http::status::ok)
    {
        resp.set_string_content(body, "text/plain"sv, status);
    }

#define UNWRAP(result)               \
    [&](auto&& r)                    \
    {                                \
        REQUIRE(r.has_value());      \
        return std::move(r).value(); \
    }(result)

} // namespace

TEST_CASE("Response: set_empty_content", "[response]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/empty",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_empty_content(http::status::no_content); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/empty"));
    REQUIRE(resp.result() == http::status::no_content);
}

TEST_CASE("Response: set_error_content", "[response]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/error",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_error_content(http::status::internal_server_error); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/error"));
    REQUIRE(resp.result() == http::status::internal_server_error);
    REQUIRE_FALSE(as_string(resp).empty());
}

TEST_CASE("Response: set_redirect", "[response]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/old",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_redirect("/new", http::status::moved_permanently); });
    ts.router().set_http_handler<http::verb::get>("/new",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { set_text(resp, "new-location"); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/old"));
    REQUIRE(resp.result() == http::status::moved_permanently);
    REQUIRE(resp[http::field::location] == "/new");
}

TEST_CASE("Response: set_chunked_write_handler with multiple chunks", "[response]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/stream",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      auto idx = std::make_shared<int>(0);
                                                      resp.set_chunked_write_handler(
                                                          [idx](httplib::chunk_writer& writer) -> net::awaitable<void>
                                                          {
                                                              constexpr std::string_view chunks[] = { "A", "B", "C" };
                                                              for (; *idx < 3; ++(*idx))
                                                              {
                                                                  co_await writer.write_chunk(
                                                                      std::string(chunks[*idx]));
                                                              }
                                                          },
                                                          "text/plain");
                                                  });
    ts.start();

    std::string streamed;
    auto writer = ts.client->create_writer();
    auto reader = ts.client->create_reader();

    boost::asio::co_spawn(ts.ioc,
                          [&]() -> net::awaitable<void>
                          {
                              co_await writer->write_header(http::verb::get, "/stream", {});
                              co_await writer->write_body(net::buffer("", 0), false);
                              co_await reader->read_header();

                              std::array<char, 4096> buf;
                              while (true)
                              {
                                  auto result = co_await reader->read_body(net::buffer(buf));
                                  if (result.has_error() || result.value() == 0)
                                  {
                                      break;
                                  }
                                  streamed.append(buf.data(), result.value());
                              }
                          },
                          boost::asio::use_future)
        .get();

    REQUIRE(reader->result() == http::status::ok);
    REQUIRE(streamed == "ABC");
}

TEST_CASE("Response: set_form_data_content", "[response]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/form-resp",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      std::vector<httplib::html::form_data::field> fields;
                                                      fields.push_back({ "name", "", "text/plain", "test-value" });
                                                      resp.set_form_data_content(std::move(fields));
                                                  });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/form-resp"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE_FALSE(resp[http::field::content_type].empty());
    REQUIRE(resp[http::field::content_type].starts_with("multipart/form-data"));
}

TEST_CASE("Response: set_form_data_content with file_path", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_form_file.bin";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "file content from disk";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/form-file",
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
    ts.start();

    auto resp = UNWRAP(ts.client->get("/form-file"));
    REQUIRE(resp.result() == http::status::ok);
    auto& fd = resp.body().as<httplib::body::form_data_body>();
    REQUIRE(fd.fields.size() == 1);
    REQUIRE(fd.fields[0].name == "file");
    REQUIRE(fd.fields[0].filename == "test.bin");
    REQUIRE(fd.fields[0].content == "file content from disk");

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: set_file_content serves a file", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_file.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "hello from test file\n";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(tmp_path); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/file"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "hello from test file\n");

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: set_file_content with Range request", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_range.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "0123456789";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file-range",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  { resp.set_file_content(tmp_path, req.base()); });
    ts.start();

    auto range_headers = httplib::http::fields();
    range_headers.set(http::field::range, "bytes=0-4");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/file-range", range_headers));
    REQUIRE(resp.result() == http::status::partial_content);
    REQUIRE(as_string(resp) == "01234");

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Range request open-ended (bytes=N-)", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_range_open.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "0123456789";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file-range-open",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  { resp.set_file_content(tmp_path, req.base()); });
    ts.start();

    auto range_headers = httplib::http::fields();
    range_headers.set(http::field::range, "bytes=7-");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/file-range-open", range_headers));
    REQUIRE(resp.result() == http::status::partial_content);
    REQUIRE(as_string(resp) == "789");

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Range request suffix (bytes=-N)", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_range_suffix.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "0123456789";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file-range-suffix",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  { resp.set_file_content(tmp_path, req.base()); });
    ts.start();

    auto range_headers = httplib::http::fields();
    range_headers.set(http::field::range, "bytes=-4");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/file-range-suffix", range_headers));
    REQUIRE(resp.result() == http::status::partial_content);
    REQUIRE(as_string(resp) == "6789");

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Range request Content-Range header", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_cr.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "abcdefghij";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file-cr",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  { resp.set_file_content(tmp_path, req.base()); });
    ts.start();

    auto range_headers = httplib::http::fields();
    range_headers.set(http::field::range, "bytes=2-5");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/file-cr", range_headers));
    REQUIRE(resp.result() == http::status::partial_content);
    REQUIRE(as_string(resp) == "cdef");
    REQUIRE(resp.base().find(http::field::content_range) != resp.base().end());
    REQUIRE_FALSE(std::string(resp[http::field::content_range]).empty());

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Range request out of bounds returns 416", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_range_oob.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "01234";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file-range-oob",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  { resp.set_file_content(tmp_path, req.base()); });
    ts.start();

    auto range_headers = httplib::http::fields();
    range_headers.set(http::field::range, "bytes=10-20");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/file-range-oob", range_headers));
    REQUIRE(resp.result() == http::status::range_not_satisfiable);
    REQUIRE(resp.base().find(http::field::content_range) != resp.base().end());

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: If-None-Match returns 304 for matching ETag", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_etag.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "etag-content";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file-etag",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  { resp.set_file_content(tmp_path, req.base()); });
    ts.start();

    // First request to get the ETag
    auto resp1 = UNWRAP(ts.client->get("/file-etag"));
    REQUIRE(resp1.result() == http::status::ok);
    REQUIRE(resp1.base().find(http::field::etag) != resp1.base().end());
    auto etag = std::string(resp1[http::field::etag]);

    // Second request with If-None-Match
    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::if_none_match, etag);
    auto resp2 = UNWRAP(ts.client->send_request(http::verb::get, "/file-etag", hdrs));
    REQUIRE(resp2.result() == http::status::not_modified);

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: If-Modified-Since returns 304 for unmodified", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_ims.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "ims-content";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file-ims",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  { resp.set_file_content(tmp_path, req.base()); });
    ts.start();

    // First request to get Last-Modified
    auto resp1 = UNWRAP(ts.client->get("/file-ims"));
    REQUIRE(resp1.result() == http::status::ok);
    REQUIRE(resp1.base().find(http::field::last_modified) != resp1.base().end());
    auto last_mod = std::string(resp1[http::field::last_modified]);

    // Second request with If-Modified-Since
    auto hdrs = httplib::http::fields();
    hdrs.set(http::field::if_modified_since, last_mod);
    auto resp2 = UNWRAP(ts.client->send_request(http::verb::get, "/file-ims", hdrs));
    REQUIRE(resp2.result() == http::status::not_modified);

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Accept-Ranges header present on full response", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_ar.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "test";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file-ar",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  { resp.set_file_content(tmp_path, req.base()); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/file-ar"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp.base().find(http::field::accept_ranges) != resp.base().end());
    REQUIRE(std::string(resp[http::field::accept_ranges]) == "bytes");

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: Multi-range request returns multipart/byteranges", "[response]")
{
    auto tmp_path = std::filesystem::temp_directory_path() / "httplib_test_multi_range.txt";
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << "0123456789";
    }

    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file-multi-range",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  { resp.set_file_content(tmp_path, req.base()); });
    ts.start();

    auto range_headers = httplib::http::fields();
    range_headers.set(http::field::range, "bytes=0-2,5-7");

    auto resp = UNWRAP(ts.client->send_request(http::verb::get, "/file-multi-range", range_headers));
    REQUIRE(resp.result() == http::status::partial_content);
    auto ct = std::string(resp[http::field::content_type]);
    REQUIRE(ct.starts_with("multipart/byteranges"));

    // The boundary is part of the content-type for multi-range responses
    REQUIRE_FALSE(ct.find("boundary=") == std::string::npos);

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Response: custom response headers", "[response]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/custom-headers",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set("X-Custom-One", "value1");
                                                      resp.set("X-Custom-Two", "value2");
                                                      set_text(resp, "ok");
                                                  });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/custom-headers"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(resp["X-Custom-One"] == "value1");
    REQUIRE(resp["X-Custom-Two"] == "value2");
}

TEST_CASE("Response: redirect 302", "[response]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/redirect-302",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_redirect("/target", http::status::found); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/redirect-302"));
    REQUIRE(resp.result() == http::status::found);
    REQUIRE(resp[http::field::location] == "/target");
}

TEST_CASE("Response: redirect 303", "[response]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/redirect-303",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_redirect("/target", http::status::see_other); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/redirect-303"));
    REQUIRE(resp.result() == http::status::see_other);
    REQUIRE(resp[http::field::location] == "/target");
}

TEST_CASE("Response: redirect 307", "[response]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/redirect-307",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_redirect("/target", http::status::temporary_redirect); });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/redirect-307"));
    REQUIRE(resp.result() == http::status::temporary_redirect);
    REQUIRE(resp[http::field::location] == "/target");
}

TEST_CASE("Response: keep-alive close response", "[response]")
{
    test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/close-conn",
                                                  [](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set(http::field::connection, "close");
                                                      resp.set_string_content("closing"sv, "text/plain"sv);
                                                  });
    ts.start();

    auto resp = UNWRAP(ts.client->get("/close-conn"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "closing");
    REQUIRE(resp[http::field::connection] == "close");
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

    test_scaffold ts;
    ts.router().set_static_mount_point("/static", tmp_dir);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/static/test.txt"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "static content");

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: returns 404 for non-existent file", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_404";
    std::filesystem::create_directories(tmp_dir);

    test_scaffold ts;
    ts.router().set_static_mount_point("/files", tmp_dir);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/files/nope.txt"));
    REQUIRE(resp.result() == http::status::not_found);

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: blocks path traversal", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_pt";
    std::filesystem::create_directories(tmp_dir);

    test_scaffold ts;
    ts.router().set_static_mount_point("/files", tmp_dir);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/files/../../../etc/passwd"));
    REQUIRE(resp.result() == http::status::bad_request);

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

    test_scaffold ts;
    ts.router().set_static_mount_point("/", tmp_dir);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "<h1>hello</h1>");

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

    auto entry = httplib::server::mount_point_entry("/dir", tmp_dir);
    entry.set_enabled_directory(true);
    entry.set_directory_format(httplib::server::mount_point_entry::dir_format_type::json);

    test_scaffold ts;
    ts.router().set_static_mount_point(std::move(entry));
    ts.start();

    auto resp = UNWRAP(ts.client->get("/dir/data/"));
    REQUIRE(resp.result() == http::status::ok);

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("Static mount: non-existent file returns 404 via mount", "[response]")
{
    auto tmp_dir = std::filesystem::temp_directory_path() / "httplib_static_no";
    std::filesystem::create_directories(tmp_dir);

    test_scaffold ts;
    ts.router().set_static_mount_point("/pub", tmp_dir);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/pub/nope.txt"));
    REQUIRE(resp.result() == http::status::not_found);

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

    test_scaffold ts;
    ts.router().set_static_mount_point("/pub", tmp_dir);
    ts.start();

    auto resp = UNWRAP(ts.client->get("/pub/sub/deep.txt"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(as_string(resp) == "nested");

    std::filesystem::remove_all(tmp_dir);
}
