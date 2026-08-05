#include "common.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/disk_cache.hpp"
#include "httplib/client/downloader.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

using namespace test_common;

namespace
{
    namespace fs = std::filesystem;

    struct dl_test_scaffold
    {
        net::io_context ioc_;
        std::thread worker_;
        httplib::server::http_server server { ioc_ };
        httplib::tcp::endpoint endpoint;
        bool started_ = false;
        std::shared_ptr<httplib::client::http_client_pool> pool;

        dl_test_scaffold()
        {
            auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            server.set_logger(std::make_shared<spdlog::logger>("httplib.tests", null_sink));
            pool = std::make_shared<httplib::client::http_client_pool>(ioc_.get_executor(), 8);
        }

        ~dl_test_scaffold()
        {
            if (started_)
            {
                pool->stop();
                server.stop().wait();
                ioc_.stop();
                if (worker_.joinable())
                {
                    worker_.join();
                }
            }
        }

        void
        start()
        {
            server.listen("127.0.0.1", 0);
            endpoint = server.local_endpoint();
            server.run();
            started_ = true;
            worker_ = std::thread([this] { ioc_.run(); });
        }

        auto&
        router()
        {
            return server.router();
        }

        std::string
        url_for_path(std::string_view path) const
        {
            return std::format("http://{}:{}{}", endpoint.address().to_string(), endpoint.port(), path);
        }
    };



    std::string
    read_file(fs::path const& path)
    {
        std::ifstream f(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
    }
} // namespace

TEST_CASE("Downloader: basic download to file", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "hello downloader\n";
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/file",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>("/file",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_length, "17");
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    auto ec = dl.download(ts.url_for_path("/file"), dl_path);
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "hello downloader\n");

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: multi-segment parallel download", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_ms_srv.bin";
    constexpr std::size_t kSize = 1024 * 100;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_ms_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/big",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>("/big",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_length, std::to_string(kSize));
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .segments = 4 });
    auto ec = dl.download(ts.url_for_path("/big"), dl_path);
    REQUIRE_FALSE(ec);

    auto content = read_file(dl_path);
    REQUIRE(content.size() == kSize);
    for (std::size_t i = 0; i < kSize; ++i)
    {
        REQUIRE(static_cast<unsigned char>(content[i]) == static_cast<unsigned char>(i % 256));
    }

    for (int i = 0; i < 8; ++i)
    {
        REQUIRE_FALSE(fs::exists(fs::path(dl_path.string() + ".part" + std::to_string(i))));
    }
    REQUIRE_FALSE(fs::exists(fs::path(dl_path.string() + ".dlstate")));

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: progress callback", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_prog_srv.bin";
    constexpr std::size_t kSize = 1024 * 50;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_prog_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/prog",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>("/prog",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_length, std::to_string(kSize));
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    std::atomic<int> call_count { 0 };
    std::atomic<std::uint64_t> last_downloaded { 0 };
    dl.set_progress_callback(
        [&](httplib::client::downloader::progress_info const& info)
        {
            ++call_count;
            REQUIRE(info.total_bytes == kSize);
            REQUIRE(info.downloaded_bytes <= kSize);
            REQUIRE(info.downloaded_bytes >= last_downloaded.load());
            last_downloaded.store(info.downloaded_bytes);
        });
    auto ec = dl.download(ts.url_for_path("/prog"), dl_path);
    REQUIRE_FALSE(ec);
    REQUIRE(call_count.load() >= 1);
    REQUIRE(last_downloaded.load() == kSize);

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: redirect follow", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_redir_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "redirected\n";
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_redir_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/start",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_redirect("/final", http::status::found); });
    ts.router().set_http_handler<http::verb::get>("/final",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>("/start",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::location, "/final");
                                                       resp.set_empty_content(http::status::found);
                                                   });
    ts.router().set_http_handler<http::verb::head>("/final",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_length, "11");
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .max_redirects = 5 });
    auto ec = dl.download(ts.url_for_path("/start"), dl_path);
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "redirected\n");

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: resume partial download", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_resume_srv.bin";
    auto dl_path = fs::temp_directory_path() / "httplib_dl_resume_out.bin";

    constexpr std::uint64_t kSize = 1024 * 60;

    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }
    {
        std::ofstream f(dl_path, std::ios::binary);
        for (std::size_t i = 0; i < 100; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/resume",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>("/resume",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_length, std::to_string(kSize));
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .resume = true });
    auto ec = dl.download(ts.url_for_path("/resume"), dl_path);
    REQUIRE_FALSE(ec);

    auto content = read_file(dl_path);
    REQUIRE(content.size() == kSize);
    for (std::size_t i = 0; i < kSize; ++i)
    {
        REQUIRE(static_cast<unsigned char>(content[i]) == static_cast<unsigned char>(i % 256));
    }

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: suggested filename from Content-Disposition", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_cd_srv.bin";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "content-disposition test\n";
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_cd_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/cd",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set(http::field::content_disposition,
                                                               R"(attachment; filename="hello.zip")");
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<http::verb::head>("/cd",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_length, "24");
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    auto ec = dl.download(ts.url_for_path("/cd"), dl_path);
    REQUIRE_FALSE(ec);
    REQUIRE(dl.suggested_filename() == "hello.zip");

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: disk cache hit on second download", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_cache_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "cached content\n";
    }

    auto dl_path1 = fs::temp_directory_path() / "httplib_dl_cache_out1.bin";
    auto dl_path2 = fs::temp_directory_path() / "httplib_dl_cache_out2.bin";
    auto cache_dir = fs::temp_directory_path() / "httplib_test_cache";

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/cached",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set(http::field::etag, "\"abc123\"");
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<http::verb::head>("/cached",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_length, "15");
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/cached"), dl_path1);
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path1) == "cached content\n");
    }
    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/cached"), dl_path2);
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path2) == "cached content\n");
    }

    REQUIRE(cache->entry_count() >= 1);

    fs::remove(server_path);
    fs::remove(dl_path1);
    fs::remove(dl_path2);
    fs::remove_all(cache_dir);
}

TEST_CASE("Downloader: cancel stops download", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_cancel_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_cancel_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/bigcancel",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>("/bigcancel",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_length, std::to_string(kSize));
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .segments = 2 });

    std::thread cancel_thread(
        [&]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            dl.cancel();
        });

    auto ec = dl.download(ts.url_for_path("/bigcancel"), dl_path);
    REQUIRE(ec);
    cancel_thread.join();

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path, rm_ec);
    for (int i = 0; i < 4; ++i)
    {
        fs::remove(fs::path(dl_path.string() + ".part" + std::to_string(i)), rm_ec);
    }
    fs::remove(fs::path(dl_path.string() + ".dlstate"), rm_ec);
}

TEST_CASE("Downloader: re-download after cancel succeeds", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_recancel_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "after-cancel\n";
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_recancel_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/recancel",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>("/recancel",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_length, "13");
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);

    dl.cancel();
    auto first = dl.download(ts.url_for_path("/recancel"), dl_path);
    REQUIRE(first);

    auto second = dl.download(ts.url_for_path("/recancel"), dl_path);
    REQUIRE_FALSE(second);
    REQUIRE(read_file(dl_path) == "after-cancel\n");

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: multiple retries succeed eventually", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_retry_srv.bin";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "retry-ok\n";
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_retry_out.bin";

    std::atomic<int> attempt { 0 };
    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/retry-me",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      int a = attempt.fetch_add(1);
                                                      if (a < 2)
                                                      {
                                                          resp.set_empty_content(http::status::internal_server_error);
                                                      }
                                                      else
                                                      {
                                                          resp.set_file_content(server_path);
                                                      }
                                                  });
    ts.router().set_http_handler<http::verb::head>("/retry-me",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(http::field::content_length, "9");
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .max_retries = 3 });
    auto ec = dl.download(ts.url_for_path("/retry-me"), dl_path);
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "retry-ok\n");
    REQUIRE(attempt.load() == 3);

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: single-segment fallback when no content-length", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_nocl_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "no content-length\n";
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_nocl_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/no-cl",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    auto ec = dl.download(ts.url_for_path("/no-cl"), dl_path);
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "no content-length\n");

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: config presets", "[downloader]")
{
    dl_test_scaffold ts;
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);

    auto& cfg = dl.get_config();
    REQUIRE(cfg.segments == 4);
    REQUIRE(cfg.max_retries == 3);
    REQUIRE(cfg.resume == true);
    REQUIRE(cfg.verify_ssl == true);
    REQUIRE(cfg.max_speed_bytes_per_sec == 0);
    REQUIRE(cfg.save_state == true);
    REQUIRE(dl.get_cache() == nullptr);

    dl.set_config({ .segments = 8, .max_retries = 5, .resume = false });
    REQUIRE(dl.get_config().segments == 8);
    REQUIRE(dl.get_config().max_retries == 5);
    REQUIRE(dl.get_config().resume == false);

    auto cache = std::make_shared<httplib::client::disk_cache>("/tmp/foo");
    dl.set_cache(cache);
    REQUIRE(dl.get_cache() == cache);
}

TEST_CASE("Downloader: custom headers sent in request", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_hdr_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "with-headers\n";
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_hdr_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/auth",
                                                  [&](httplib::server::request& req,
                                                      httplib::server::response& resp)
                                                  {
                                                      auto auth = req[http::field::authorization];
                                                      if (auth != "Bearer secret-token")
                                                      {
                                                          resp.set_empty_content(http::status::forbidden);
                                                          return;
                                                      }
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<http::verb::head>("/auth",
                                                   [&](httplib::server::request& req,
                                                       httplib::server::response& resp)
                                                   {
                                                       auto auth = req[http::field::authorization];
                                                       if (auth != "Bearer secret-token")
                                                       {
                                                           resp.set_empty_content(http::status::forbidden);
                                                           return;
                                                       }
                                                       resp.set(http::field::content_length, "13");
                                                       resp.set(http::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    http::fields headers;
    headers.set(http::field::authorization, "Bearer secret-token");
    auto ec = dl.download(ts.url_for_path("/auth"), dl_path, headers);
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "with-headers\n");

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: disk_cache basic put and get", "[downloader]")
{
    auto tmp = fs::temp_directory_path() / "httplib_dl_cache_basic";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    auto src = tmp / "src.bin";
    {
        std::ofstream f(src, std::ios::binary);
        std::string data = "hello cache\n";
        f.write(data.data(), data.size());
    }

    httplib::client::disk_cache cache(tmp);

    http::fields headers;
    headers.set(http::field::etag, "\"abc\"");
    headers.set(http::field::content_type, "text/plain");
    cache.put("http://example.com/test.txt", headers, src);

    auto entry = cache.get("http://example.com/test.txt");
    REQUIRE(entry.has_value());
    REQUIRE(entry->body_size > 0);
    REQUIRE(entry->headers[http::field::etag] == "\"abc\"");
    REQUIRE(entry->headers[http::field::content_type] == "text/plain");

    auto miss = cache.get("http://example.com/other.txt");
    REQUIRE_FALSE(miss.has_value());

    cache.remove("http://example.com/test.txt");
    REQUIRE_FALSE(cache.get("http://example.com/test.txt").has_value());

    cache.put("http://a.com/1", headers, src);
    cache.put("http://a.com/2", headers, src);
    REQUIRE(cache.get("http://a.com/1").has_value());
    REQUIRE(cache.get("http://a.com/2").has_value());

    cache.clear();
    REQUIRE_FALSE(cache.get("http://a.com/1").has_value());

    fs::remove_all(tmp);
}
