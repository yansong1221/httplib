#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include "httplib/version.hpp"
#include <boost/asio/thread_pool.hpp>
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <thread>

using namespace std::string_view_literals;
namespace fs = std::filesystem;
namespace http = httplib::http;
namespace net = httplib::net;

static void
generate_file(fs::path const& path, std::uint64_t size_bytes, bool compressible)
{
    std::ofstream f(path, std::ios::binary);
    std::string chunk(1024 * 1024, '\0');
    if (compressible)
    {
        for (std::size_t i = 0; i < chunk.size(); ++i)
        {
            chunk[i] = static_cast<char>('a' + (i % 26));
        }
    }
    else
    {
        std::mt19937 rng(42);
        for (auto& c : chunk)
        {
            c = static_cast<char>(rng() & 0xff);
        }
    }
    std::uint64_t remaining = size_bytes;
    while (remaining > 0)
    {
        auto n = (std::min)(static_cast<std::uint64_t>(chunk.size()), remaining);
        f.write(chunk.data(), static_cast<std::streamsize>(n));
        remaining -= n;
    }
}

int
main(int argc, char* argv[])
{
    uint16_t port = 8080;
    std::uint64_t size_mb = 200;
    std::string host = "0.0.0.0";

    for (int i = 1; i < argc; ++i)
    {
        std::string_view a = argv[i];
        if (a == "--port" && i + 1 < argc)
        {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if (a == "--size" && i + 1 < argc)
        {
            size_mb = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (a == "--host" && i + 1 < argc)
        {
            host = argv[++i];
        }
        else if (a == "--help" || a == "-h")
        {
            std::printf("usage: download_demo [--host 0.0.0.0] [--port 8080] [--size 200]\n");
            return 0;
        }
    }

    auto tmp = fs::temp_directory_path();
    auto bin_path = tmp / "download_test.bin";
    auto txt_path = tmp / "download_test.txt";
    std::uint64_t size_bytes = size_mb * 1024 * 1024;

    spdlog::info("generating test files ({} MB each)...", size_mb);
    generate_file(bin_path, size_bytes, false);
    generate_file(txt_path, size_bytes, true);

    net::thread_pool pool(std::thread::hardware_concurrency());
    httplib::server::http_server svr(pool.get_executor());
    svr.logger()->set_level(spdlog::level::info);
    svr.listen(host, port);

    auto& router = svr.router();

    router.set_http_handler<http::verb::get>(
        "/",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            std::string html = std::format(
                "<html><body>"
                "<h1>File Download Demo</h1>"
                "<ul>"
                "<li><a href=\"/download\">download.bin ({} MB, octet-stream, not compressed)</a></li>"
                "<li><a href=\"/download.txt\">download.txt ({} MB, text/plain, compressed)</a></li>"
                "</ul>"
                "</body></html>",
                size_mb,
                size_mb);
            resp.set_string_content(std::move(html), "text/html; charset=utf-8");
        });

    router.set_http_handler<http::verb::get>(
        "/download",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set_file_content(bin_path);
            resp.set(http::field::content_disposition, "attachment; filename=download.bin");
        });

    router.set_http_handler<http::verb::get>(
        "/download.txt",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set_file_content(txt_path);
            resp.set(http::field::content_disposition, "attachment; filename=download.txt");
        });

    auto ep = svr.local_endpoint();
    spdlog::info("server listening on http://{}:{}", ep.address().to_string(), ep.port());
    spdlog::info("open in browser: http://<your-host>:{}/", ep.port());

    svr.run();
    pool.wait();
    return 0;
}
