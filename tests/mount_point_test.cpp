#include "common.hpp"
#include "httplib/server/mount_point_entry.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/json/value.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

using test_common::run;

namespace
{
    class temporary_dir
    {
      public:
        explicit temporary_dir(std::string_view name)
            : path_(std::filesystem::temp_directory_path() / std::string(name))
        {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
            std::filesystem::create_directories(path_, ec);
        }

        ~temporary_dir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }

        temporary_dir(temporary_dir const&) = delete;
        temporary_dir& operator=(temporary_dir const&) = delete;

        std::filesystem::path const&
        path() const
        {
            return path_;
        }

      private:
        std::filesystem::path path_;
    };

    void
    write_file(std::filesystem::path const& path, std::string_view content)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        std::ofstream file(path, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    struct header_aspect
    {
        bool
        after(httplib::server::request&, httplib::server::response& resp)
        {
            resp.set("x-mount-aspect", "1");
            return true;
        }
    };
} // namespace

TEST_CASE("Static mount: serves files and rejects missing paths", "[mount]")
{
    temporary_dir tmp("httplib_mount_files");
    write_file(tmp.path() / "test.txt", "static content");
    write_file(tmp.path() / "sub" / "deep.txt", "nested");
    write_file(tmp.path() / "a b.txt", "space");

    run([&](auto& server) { server.router().set_static_mount_point("/static", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            auto file = UNWRAP(co_await client.async_get("/static/test.txt"));
            REQUIRE(file.result() == httplib::status::ok);
            REQUIRE(file.as_string() == "static content");
            REQUIRE(file[httplib::field::content_type].starts_with("text/plain"));
            REQUIRE(file[httplib::field::accept_ranges] == "bytes");

            auto nested = UNWRAP(co_await client.async_get("/static/sub/deep.txt"));
            REQUIRE(nested.result() == httplib::status::ok);
            REQUIRE(nested.as_string() == "nested");

            // 文件名的编码必须先逐段解码，再拼回文件系统路径。
            auto encoded_space = UNWRAP(co_await client.async_get("/static/a%20b.txt"));
            REQUIRE(encoded_space.result() == httplib::status::ok);
            REQUIRE(encoded_space.as_string() == "space");

            auto missing = UNWRAP(co_await client.async_get("/static/nope.txt"));
            REQUIRE(missing.result() == httplib::status::not_found);

            auto missing_dir = UNWRAP(co_await client.async_get("/static/nope/"));
            REQUIRE(missing_dir.result() == httplib::status::not_found);

            auto file_with_slash = UNWRAP(co_await client.async_get("/static/test.txt/"));
            REQUIRE(file_with_slash.result() == httplib::status::not_found);

            co_return;
        });
}

TEST_CASE("Static mount: rejects traversal and encoded separators", "[mount]")
{
    temporary_dir tmp("httplib_mount_security");
    write_file(tmp.path() / "a" / "b.txt", "nested");
    write_file(tmp.path() / "b.txt", "root");

    run([&](auto& server) { server.router().set_static_mount_point("/files", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            // 真实的两段路径正常访问。
            auto normal = UNWRAP(co_await client.async_get("/files/a/b.txt"));
            REQUIRE(normal.result() == httplib::status::ok);
            REQUIRE(normal.as_string() == "nested");

            // 内部 ".." 仍在 mount 根内，保持原有解析语义。
            auto internal_dotdot = UNWRAP(co_await client.async_get("/files/a/../b.txt"));
            REQUIRE(internal_dotdot.result() == httplib::status::ok);
            REQUIRE(internal_dotdot.as_string() == "root");

            // 再上一层就越过 mount 根，必须拒绝。
            auto over_root = UNWRAP(co_await client.async_get("/files/a/../../b.txt"));
            REQUIRE(over_root.result() == httplib::status::bad_request);

            // 段内 %2F 是内容，不是目录层级。
            auto encoded_sep = UNWRAP(co_await client.async_get("/files/a%2Fb.txt"));
            REQUIRE(encoded_sep.result() == httplib::status::bad_request);

            // mount 前缀里的 %2F 不会增加匹配段数。
            auto mounted_encoded = UNWRAP(co_await client.async_get("/files%2Fb.txt"));
            REQUIRE(mounted_encoded.result() == httplib::status::not_found);

            auto traversal = UNWRAP(co_await client.async_get("/files/../../../etc/passwd"));
            REQUIRE(traversal.result() == httplib::status::bad_request);

            auto encoded_dotdot = UNWRAP(co_await client.async_get("/files/%2E%2E/secret"));
            REQUIRE(encoded_dotdot.result() == httplib::status::bad_request);

            auto encoded_dotdot_slash = UNWRAP(co_await client.async_get("/files/%2E%2E%2Fsecret"));
            REQUIRE(encoded_dotdot_slash.result() == httplib::status::bad_request);

            auto backslash = UNWRAP(co_await client.async_get("/files/%5C"));
            REQUIRE(backslash.result() == httplib::status::bad_request);

            auto nul = UNWRAP(co_await client.async_get("/files/%00"));
            REQUIRE(nul.result() == httplib::status::bad_request);

            co_return;
        });
}

TEST_CASE("Static mount: redirects existing directories only", "[mount]")
{
    temporary_dir tmp("httplib_mount_redirect");
    write_file(tmp.path() / "data" / "a.txt", "a");
    write_file(tmp.path() / "file.txt", "file");
    write_file(tmp.path() / "a b" / "x.txt", "x");

    run([&](auto& server) { server.router().set_static_mount_point("/static", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            auto dir = UNWRAP(co_await client.async_get("/static/data"));
            REQUIRE(dir.result() == httplib::status::moved_permanently);
            REQUIRE(dir[httplib::field::location] == "/static/data/");

            // 重定向保留查询串。
            auto dir_query = UNWRAP(co_await client.async_get("/static/data?x=1"));
            REQUIRE(dir_query.result() == httplib::status::moved_permanently);
            REQUIRE(dir_query[httplib::field::location] == "/static/data/?x=1");

            // HEAD 目录请求同样补斜杠。
            auto dir_head = UNWRAP(co_await client.async_head("/static/data"));
            REQUIRE(dir_head.result() == httplib::status::moved_permanently);
            REQUIRE(dir_head[httplib::field::location] == "/static/data/");

            // 原始编码保持不变，不能把 %20 解码后写进 Location。
            auto encoded_dir = UNWRAP(co_await client.async_get("/static/a%20b"));
            REQUIRE(encoded_dir.result() == httplib::status::moved_permanently);
            REQUIRE(encoded_dir[httplib::field::location] == "/static/a%20b/");

            // 不存在的目录不能靠补斜杠变成 301。
            auto missing = UNWRAP(co_await client.async_get("/static/missing"));
            REQUIRE(missing.result() == httplib::status::not_found);
            REQUIRE(missing[httplib::field::location].empty());

            // 普通文件仍直接返回。
            auto file = UNWRAP(co_await client.async_get("/static/file.txt"));
            REQUIRE(file.result() == httplib::status::ok);
            REQUIRE(file.as_string() == "file");

            auto file_slash = UNWRAP(co_await client.async_get("/static/file.txt/"));
            REQUIRE(file_slash.result() == httplib::status::not_found);

            auto dir_slash = UNWRAP(co_await client.async_get("/static/data/"));
            REQUIRE(dir_slash.result() == httplib::status::ok);

            auto encoded_dir_slash = UNWRAP(co_await client.async_get("/static/a%20b/"));
            REQUIRE(encoded_dir_slash.result() == httplib::status::ok);

            co_return;
        });
}

TEST_CASE("Static mount: normalizes mount prefix and supports multiple segments", "[mount]")
{
    temporary_dir tmp("httplib_mount_prefix");
    write_file(tmp.path() / "data" / "file.txt", "payload");

    run([&](auto& server) { server.router().set_static_mount_point("/assets/v1/", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            auto file = UNWRAP(co_await client.async_get("/assets/v1/data/file.txt"));
            REQUIRE(file.result() == httplib::status::ok);
            REQUIRE(file.as_string() == "payload");

            auto dir = UNWRAP(co_await client.async_get("/assets/v1/data"));
            REQUIRE(dir.result() == httplib::status::moved_permanently);
            REQUIRE(dir[httplib::field::location] == "/assets/v1/data/");

            auto dir_slash = UNWRAP(co_await client.async_get("/assets/v1/data/"));
            REQUIRE(dir_slash.result() == httplib::status::ok);

            co_return;
        });
}

TEST_CASE("Static mount: root mount redirects subdirectories", "[mount]")
{
    temporary_dir tmp("httplib_mount_root");
    write_file(tmp.path() / "data" / "file.txt", "payload");

    run([&](auto& server) { server.router().set_static_mount_point("/", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            auto dir = UNWRAP(co_await client.async_get("/data"));
            REQUIRE(dir.result() == httplib::status::moved_permanently);
            REQUIRE(dir[httplib::field::location] == "/data/");

            auto dir_slash = UNWRAP(co_await client.async_get("/data/"));
            REQUIRE(dir_slash.result() == httplib::status::ok);

            auto file = UNWRAP(co_await client.async_get("/data/file.txt"));
            REQUIRE(file.result() == httplib::status::ok);
            REQUIRE(file.as_string() == "payload");

            co_return;
        });
}

TEST_CASE("Static mount: root mount serves default document", "[mount]")
{
    temporary_dir tmp("httplib_mount_root_doc");
    write_file(tmp.path() / "index.html", "<h1>root</h1>");

    run([&](auto& server) { server.router().set_static_mount_point("/", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "<h1>root</h1>");
            co_return;
        });
}

TEST_CASE("Static mount: empty mount point behaves as root", "[mount]")
{
    temporary_dir tmp("httplib_mount_empty");
    write_file(tmp.path() / "file.txt", "payload");

    run([&](auto& server) { server.router().set_static_mount_point("", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/file.txt"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "payload");
            co_return;
        });
}

TEST_CASE("Static mount: aspects run around file response", "[mount]")
{
    temporary_dir tmp("httplib_mount_aspect");
    write_file(tmp.path() / "file.txt", "payload");

    run([&](auto& server) { server.router().set_static_mount_point("/files", tmp.path(), header_aspect {}); },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/files/file.txt"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "payload");
            REQUIRE(resp["x-mount-aspect"] == "1");
            co_return;
        });
}

TEST_CASE("Static mount: default documents in subdirectories", "[mount]")
{
    temporary_dir tmp("httplib_mount_default_doc");
    write_file(tmp.path() / "site-a" / "index.html", "index html");
    write_file(tmp.path() / "site-b" / "index.htm", "index htm");
    write_file(tmp.path() / "site-c" / "home.html", "home html");

    run(
        [&](auto& server)
        {
            server.router().set_static_mount_point("/a", tmp.path());
            server.router().set_static_mount_point("/b", tmp.path());

            auto custom = httplib::server::mount_point_entry("/c", tmp.path());
            custom.set_default_document_name({ "home.html" });
            server.router().set_static_mount_point(std::move(custom));
        },
        [](auto& client) -> net::awaitable<void>
        {
            // 目录存在但 URL 无尾斜杠时先 301，再进入默认文档。
            auto redirect = UNWRAP(co_await client.async_get("/a/site-a"));
            REQUIRE(redirect.result() == httplib::status::moved_permanently);
            REQUIRE(redirect[httplib::field::location] == "/a/site-a/");

            auto index_html = UNWRAP(co_await client.async_get("/a/site-a/"));
            REQUIRE(index_html.result() == httplib::status::ok);
            REQUIRE(index_html.as_string() == "index html");

            auto index_htm = UNWRAP(co_await client.async_get("/b/site-b/"));
            REQUIRE(index_htm.result() == httplib::status::ok);
            REQUIRE(index_htm.as_string() == "index htm");

            auto custom = UNWRAP(co_await client.async_get("/c/site-c/"));
            REQUIRE(custom.result() == httplib::status::ok);
            REQUIRE(custom.as_string() == "home html");

            co_return;
        });
}

TEST_CASE("Static mount: directory listing can be disabled", "[mount]")
{
    temporary_dir tmp("httplib_mount_no_listing");
    write_file(tmp.path() / "empty" / "keep.txt", "x");
    write_file(tmp.path() / "with-index" / "index.html", "index");
    write_file(tmp.path() / "file.txt", "file");

    run(
        [&](auto& server)
        {
            auto entry = httplib::server::mount_point_entry("/files", tmp.path());
            entry.set_enabled_directory(false);
            server.router().set_static_mount_point(std::move(entry));
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto listing = UNWRAP(co_await client.async_get("/files/empty/"));
            REQUIRE(listing.result() == httplib::status::forbidden);

            // 目录存在时，关闭列表也仍然先补尾斜杠。
            auto redirect = UNWRAP(co_await client.async_get("/files/empty"));
            REQUIRE(redirect.result() == httplib::status::moved_permanently);
            REQUIRE(redirect[httplib::field::location] == "/files/empty/");

            // 关闭目录列表不影响默认文档。
            auto index = UNWRAP(co_await client.async_get("/files/with-index/"));
            REQUIRE(index.result() == httplib::status::ok);
            REQUIRE(index.as_string() == "index");

            // 关闭目录列表也不影响普通文件。
            auto file = UNWRAP(co_await client.async_get("/files/file.txt"));
            REQUIRE(file.result() == httplib::status::ok);
            REQUIRE(file.as_string() == "file");

            co_return;
        });
}

TEST_CASE("Static mount: JSON directory listing", "[mount]")
{
    temporary_dir tmp("httplib_mount_json");
    write_file(tmp.path() / "data" / "a.txt", "a");
    std::filesystem::create_directories(tmp.path() / "data" / "sub");
    std::filesystem::create_directories(tmp.path() / "empty");

    run(
        [&](auto& server)
        {
            auto entry = httplib::server::mount_point_entry("/files", tmp.path());
            entry.set_directory_format(httplib::server::mount_point_entry::dir_format_type::json);
            server.router().set_static_mount_point(std::move(entry));
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/files/data/"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp[httplib::field::content_type].starts_with("application/json"));

            auto const& entries = resp.as_json().as_array();
            REQUIRE(entries.size() == 2);

            bool saw_file = false;
            bool saw_dir = false;
            for (auto const& entry : entries)
            {
                auto name = std::string(entry.at("filename").as_string());
                if (name == "a.txt")
                {
                    saw_file = true;
                    REQUIRE_FALSE(entry.at("is_dir").as_bool());
                    REQUIRE(entry.at("filesize").as_int64() == 1);
                }
                else if (name == "sub")
                {
                    saw_dir = true;
                    REQUIRE(entry.at("is_dir").as_bool());
                }
            }
            REQUIRE(saw_file);
            REQUIRE(saw_dir);

            // 空目录列表是合法响应，不是 404。
            auto empty = UNWRAP(co_await client.async_get("/files/empty/"));
            REQUIRE(empty.result() == httplib::status::ok);
            REQUIRE(empty.as_json().as_array().empty());

            co_return;
        });
}

TEST_CASE("Static mount: html directory listing escapes target and file names", "[mount]")
{
    temporary_dir tmp("httplib_mount_html");
    write_file(tmp.path() / "a&b.txt", "x");

    run(
        [&](auto& server)
        {
            // mount point 故意包含 '<' '>'：目录列表的 target 来自请求路径，
            // 必须被 HTML 转义，不能原样进入 <title>/<h1>。
            auto entry = httplib::server::mount_point_entry("/<x>", tmp.path());
            entry.set_enabled_directory(true);
            entry.set_directory_format(httplib::server::mount_point_entry::dir_format_type::html);
            server.router().set_static_mount_point(std::move(entry));
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/<x>/"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp[httplib::field::content_type] == "text/html; charset=utf-8");

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

TEST_CASE("Static mount: symlink inside base is served", "[mount]")
{
    temporary_dir tmp("httplib_mount_symlink_in");
    write_file(tmp.path() / "target.txt", "inside");

    std::error_code ec;
    std::filesystem::create_symlink(tmp.path() / "target.txt", tmp.path() / "link.txt", ec);
    if (ec)
    {
        SKIP("symlink creation not permitted");
    }

    run([&](auto& server) { server.router().set_static_mount_point("/files", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/files/link.txt"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "inside");
            co_return;
        });
}

TEST_CASE("Static mount: symlink to directory inside base is served", "[mount]")
{
    temporary_dir tmp("httplib_mount_symlink_dir");
    write_file(tmp.path() / "real" / "target.txt", "inside");

    std::error_code ec;
    std::filesystem::create_directory_symlink(tmp.path() / "real", tmp.path() / "linkdir", ec);
    if (ec)
    {
        SKIP("symlink creation not permitted");
    }

    run([&](auto& server) { server.router().set_static_mount_point("/files", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            auto file = UNWRAP(co_await client.async_get("/files/linkdir/target.txt"));
            REQUIRE(file.result() == httplib::status::ok);
            REQUIRE(file.as_string() == "inside");

            auto dir = UNWRAP(co_await client.async_get("/files/linkdir"));
            REQUIRE(dir.result() == httplib::status::moved_permanently);
            REQUIRE(dir[httplib::field::location] == "/files/linkdir/");

            auto listing = UNWRAP(co_await client.async_get("/files/linkdir/"));
            REQUIRE(listing.result() == httplib::status::ok);

            co_return;
        });
}

TEST_CASE("Static mount: symlink escaping base is forbidden", "[mount]")
{
    temporary_dir tmp("httplib_mount_symlink_out");
    temporary_dir outside("httplib_mount_symlink_target");
    write_file(outside.path() / "secret.txt", "secret");

    std::error_code ec;
    std::filesystem::create_symlink(outside.path() / "secret.txt", tmp.path() / "link.txt", ec);
    if (ec)
    {
        SKIP("symlink creation not permitted");
    }

    run([&](auto& server) { server.router().set_static_mount_point("/files", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/files/link.txt"));
            REQUIRE(resp.result() == httplib::status::forbidden);
            co_return;
        });
}

TEST_CASE("Static mount: directory symlink escaping base is forbidden", "[mount]")
{
    temporary_dir tmp("httplib_mount_symlink_dir_out");
    temporary_dir outside("httplib_mount_symlink_dir_target");
    write_file(outside.path() / "secret.txt", "secret");

    std::error_code ec;
    std::filesystem::create_directory_symlink(outside.path(), tmp.path() / "linkdir", ec);
    if (ec)
    {
        SKIP("symlink creation not permitted");
    }

    run([&](auto& server) { server.router().set_static_mount_point("/files", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            auto with_slash = UNWRAP(co_await client.async_get("/files/linkdir/"));
            REQUIRE(with_slash.result() == httplib::status::forbidden);

            auto without_slash = UNWRAP(co_await client.async_get("/files/linkdir"));
            REQUIRE(without_slash.result() == httplib::status::forbidden);

            co_return;
        });
}

TEST_CASE("Static mount: HEAD, ranges, and conditionals", "[mount]")
{
    temporary_dir tmp("httplib_mount_cache");
    write_file(tmp.path() / "file.txt", "0123456789");
    write_file(tmp.path() / "file.bin", "0123456789");

    run([&](auto& server) { server.router().set_static_mount_point("/files", tmp.path()); },
        [](auto& client) -> net::awaitable<void>
        {
            // 非压缩内容类型走普通 Content-Length 分帧，HEAD 头可以直接断言。
            auto bin_head = UNWRAP(co_await client.async_head("/files/file.bin"));
            REQUIRE(bin_head.result() == httplib::status::ok);
            REQUIRE(bin_head.as_string().empty());
            REQUIRE(bin_head[httplib::field::content_encoding].empty());
            REQUIRE(bin_head[httplib::field::content_length] == "10");

            auto head = UNWRAP(co_await client.async_head("/files/file.txt"));
            REQUIRE(head.result() == httplib::status::ok);
            REQUIRE(head.as_string().empty());
            REQUIRE(head[httplib::field::accept_ranges] == "bytes");
            REQUIRE(head.base().has(httplib::field::etag));
            REQUIRE(head.base().has(httplib::field::last_modified));

            auto etag = std::string(head[httplib::field::etag]);
            auto last_modified = std::string(head[httplib::field::last_modified]);

            auto range_headers = httplib::headers();
            range_headers.set(httplib::field::range, "bytes=0-4");
            httplib::client::request range_req(httplib::method::get, "/files/file.txt", range_headers);
            auto range = UNWRAP(co_await client.async_send_request(range_req));
            REQUIRE(range.result() == httplib::status::partial_content);
            REQUIRE(range.as_string() == "01234");
            REQUIRE(range[httplib::field::content_range] == "bytes 0-4/10");

            auto open_headers = httplib::headers();
            open_headers.set(httplib::field::range, "bytes=7-");
            httplib::client::request open_req(httplib::method::get, "/files/file.txt", open_headers);
            auto open = UNWRAP(co_await client.async_send_request(open_req));
            REQUIRE(open.result() == httplib::status::partial_content);
            REQUIRE(open.as_string() == "789");

            auto suffix_headers = httplib::headers();
            suffix_headers.set(httplib::field::range, "bytes=-3");
            httplib::client::request suffix_req(httplib::method::get, "/files/file.txt", suffix_headers);
            auto suffix = UNWRAP(co_await client.async_send_request(suffix_req));
            REQUIRE(suffix.result() == httplib::status::partial_content);
            REQUIRE(suffix.as_string() == "789");

            auto multi_headers = httplib::headers();
            multi_headers.set(httplib::field::range, "bytes=0-1,4-5");
            httplib::client::request multi_req(httplib::method::get, "/files/file.txt", multi_headers);
            auto multi = UNWRAP(co_await client.async_send_request(multi_req));
            REQUIRE(multi.result() == httplib::status::partial_content);
            REQUIRE(multi[httplib::field::content_type].starts_with("multipart/byteranges; boundary="));
            REQUIRE(multi.as_string().find("Content-Range: bytes 0-1/10") != std::string::npos);

            auto head_range_headers = httplib::headers();
            head_range_headers.set(httplib::field::range, "bytes=0-4");
            httplib::client::request head_range_req(httplib::method::head, "/files/file.txt", head_range_headers);
            auto head_range = UNWRAP(co_await client.async_send_request(head_range_req));
            REQUIRE(head_range.result() == httplib::status::partial_content);
            REQUIRE(head_range.as_string().empty());
            REQUIRE(head_range[httplib::field::content_range] == "bytes 0-4/10");

            auto oob_headers = httplib::headers();
            oob_headers.set(httplib::field::range, "bytes=99-");
            httplib::client::request oob_req(httplib::method::get, "/files/file.txt", oob_headers);
            auto oob = UNWRAP(co_await client.async_send_request(oob_req));
            REQUIRE(oob.result() == httplib::status::range_not_satisfiable);
            REQUIRE(oob[httplib::field::content_range] == "bytes */10");

            auto inm_headers = httplib::headers();
            inm_headers.set(httplib::field::if_none_match, etag);
            httplib::client::request inm_req(httplib::method::get, "/files/file.txt", inm_headers);
            auto inm = UNWRAP(co_await client.async_send_request(inm_req));
            REQUIRE(inm.result() == httplib::status::not_modified);

            auto ims_headers = httplib::headers();
            ims_headers.set(httplib::field::if_modified_since, last_modified);
            httplib::client::request ims_req(httplib::method::get, "/files/file.txt", ims_headers);
            auto ims = UNWRAP(co_await client.async_send_request(ims_req));
            REQUIRE(ims.result() == httplib::status::not_modified);

            co_return;
        });
}

TEST_CASE("Static mount: copied entry keeps normalized prefix", "[mount]")
{
    temporary_dir tmp("httplib_mount_copy");
    write_file(tmp.path() / "file.txt", "payload");

    run(
        [&](auto& server)
        {
            auto original = httplib::server::mount_point_entry("/static/", tmp.path());
            auto copied = original;
            server.router().set_static_mount_point(std::move(copied));
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/static/file.txt"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp.as_string() == "payload");
            co_return;
        });
}
