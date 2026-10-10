#include "common.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/disk_cache.hpp"
#include "httplib/client/downloader.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <atomic>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/errc.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
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
            pool = std::make_shared<httplib::client::http_client_pool>(ioc_.get_executor(),
                                                                       httplib::client::pool_params { .max_size = 8 });
        }

        ~dl_test_scaffold()
        {
            if (started_)
            {
                pool->stop();
                server.stop();
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
    ts.router().set_http_handler<httplib::method::get>("/file",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<httplib::method::head>("/file",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::content_length, "17");
                                                       resp.set(httplib::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    auto ec = dl.download(ts.url_for_path("/file"), dl_path).get();
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "hello downloader\n");

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
    ts.router().set_http_handler<httplib::method::get>("/prog",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<httplib::method::head>("/prog",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::content_length, std::to_string(kSize));
                                                       resp.set(httplib::field::accept_ranges, "bytes");
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
    auto ec = dl.download(ts.url_for_path("/prog"), dl_path).get();
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
    ts.router().set_http_handler<httplib::method::get>("/start",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_redirect("/final", httplib::status::found); });
    ts.router().set_http_handler<httplib::method::get>("/final",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<httplib::method::head>("/start",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::location, "/final");
                                                       resp.set_empty_content(httplib::status::found);
                                                   });
    ts.router().set_http_handler<httplib::method::head>("/final",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::content_length, "11");
                                                       resp.set(httplib::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .max_redirects = 5 });
    auto ec = dl.download(ts.url_for_path("/start"), dl_path).get();
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "redirected\n");

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: strips sensitive headers on cross-origin redirect", "[downloader]")
{
    auto srv_path = fs::temp_directory_path() / "httplib_dl_cross_srv.txt";
    {
        std::ofstream f(srv_path, std::ios::binary);
        f << "cross-origin-ok\n";
    }
    auto dl_path = fs::temp_directory_path() / "httplib_dl_cross_out.bin";

    net::io_context ioc;
    httplib::server::http_server target { ioc };
    httplib::server::http_server origin { ioc };
    std::thread worker;
    std::atomic<bool> leaked_auth = false;
    std::atomic<bool> leaked_cookie = false;
    std::atomic<int> target_hits = 0;
    auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();

    target.set_logger(std::make_shared<spdlog::logger>("t", null_sink));
    origin.set_logger(std::make_shared<spdlog::logger>("o", null_sink));
    target.router().set_http_handler<httplib::method::get>(
        "/target",
        [&](httplib::server::request& req, httplib::server::response& resp)
        {
            ++target_hits;
            leaked_auth = req.has(httplib::field::authorization);
            leaked_cookie = req.has(httplib::field::cookie);
            resp.set_file_content(srv_path);
        });
    target.router().set_http_handler<httplib::method::head>(
        "/target",
        [&](httplib::server::request& req, httplib::server::response& resp)
        {
            leaked_auth = req.has(httplib::field::authorization);
            leaked_cookie = req.has(httplib::field::cookie);
            resp.set(httplib::field::content_length, "17");
            resp.set(httplib::field::accept_ranges, "bytes");
        });

    target.listen("127.0.0.1", 0);
    auto target_port = target.local_endpoint().port();
    origin.router().set_http_handler<httplib::method::get>(
        "/start",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_redirect(std::format("http://127.0.0.1:{}/target", target_port), httplib::status::found); });
    origin.router().set_http_handler<httplib::method::head>(
        "/start",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(httplib::field::location, std::format("http://127.0.0.1:{}/target", target_port));
            resp.set_empty_content(httplib::status::found);
        });
    origin.listen("127.0.0.1", 0);

    auto pool = std::make_shared<httplib::client::http_client_pool>(ioc.get_executor(),
                                                                    httplib::client::pool_params { .max_size = 8 });

    target.run();
    origin.run();
    worker = std::thread([&] { ioc.run(); });

    httplib::client::downloader dl(ioc, pool);
    httplib::headers headers;
    headers.set(httplib::field::authorization, "Bearer secret");
    headers.set(httplib::field::cookie, "session=abc");
    dl.set_config({ .max_redirects = 5 });
    auto ec
        = dl.download(std::format("http://127.0.0.1:{}/start", origin.local_endpoint().port()), dl_path, headers).get();
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "cross-origin-ok\n");
    REQUIRE(target_hits.load() >= 1);
    REQUIRE_FALSE(leaked_auth.load());
    REQUIRE_FALSE(leaked_cookie.load());

    pool->stop();
    origin.stop();
    target.stop();
    ioc.stop();
    worker.join();

    fs::remove(srv_path);
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
    ts.router().set_http_handler<httplib::method::get>("/resume",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<httplib::method::head>("/resume",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::content_length, std::to_string(kSize));
                                                       resp.set(httplib::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .resume = true });
    auto ec = dl.download(ts.url_for_path("/resume"), dl_path).get();
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
    ts.router().set_http_handler<httplib::method::get>("/cd",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set(httplib::field::content_disposition,
                                                               R"(attachment; filename="hello.zip")");
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/cd",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   { resp.set_file_content(server_path); });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    auto ec = dl.download(ts.url_for_path("/cd"), dl_path).get();
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
    ts.router().set_http_handler<httplib::method::get>("/cached",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set(httplib::field::etag, "\"abc123\"");
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/cached",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::content_length, "15");
                                                       resp.set(httplib::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/cached"), dl_path1).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path1) == "cached content\n");
    }
    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/cached"), dl_path2).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path2) == "cached content\n");
    }

    REQUIRE(cache->entry_count() >= 1);

    fs::remove(server_path);
    fs::remove(dl_path1);
    fs::remove(dl_path2);
    fs::remove_all(cache_dir);
}

TEST_CASE("Downloader: cache isolates different origins", "[downloader]")
{
    auto server_path_a = fs::temp_directory_path() / "httplib_dl_cache_origin_a.txt";
    auto server_path_b = fs::temp_directory_path() / "httplib_dl_cache_origin_b.txt";
    {
        std::ofstream fa(server_path_a, std::ios::binary);
        fa << "content from server A\n";
    }
    {
        std::ofstream fb(server_path_b, std::ios::binary);
        fb << "content from server B\n";
    }

    auto dl_path_a = fs::temp_directory_path() / "httplib_dl_cache_iso_a.bin";
    auto dl_path_b = fs::temp_directory_path() / "httplib_dl_cache_iso_b.bin";
    auto cache_dir = fs::temp_directory_path() / "httplib_test_cache_iso";

    dl_test_scaffold ts_a;
    ts_a.router().set_http_handler<httplib::method::get>("/data",
                                                    [&](httplib::server::request&, httplib::server::response& resp)
                                                    {
                                                        resp.set(httplib::field::etag, "\"aaa\"");
                                                        resp.set_file_content(server_path_a);
                                                    });
    ts_a.router().set_http_handler<httplib::method::head>("/data",
                                                     [&](httplib::server::request&, httplib::server::response& resp)
                                                     {
                                                         resp.set(httplib::field::content_length, "22");
                                                         resp.set(httplib::field::accept_ranges, "bytes");
                                                     });
    ts_a.start();

    dl_test_scaffold ts_b;
    ts_b.router().set_http_handler<httplib::method::get>("/data",
                                                    [&](httplib::server::request&, httplib::server::response& resp)
                                                    {
                                                        resp.set(httplib::field::etag, "\"bbb\"");
                                                        resp.set_file_content(server_path_b);
                                                    });
    ts_b.router().set_http_handler<httplib::method::head>("/data",
                                                     [&](httplib::server::request&, httplib::server::response& resp)
                                                     {
                                                         resp.set(httplib::field::content_length, "22");
                                                         resp.set(httplib::field::accept_ranges, "bytes");
                                                     });
    ts_b.start();

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);

    {
        httplib::client::downloader dl(ts_a.ioc_, ts_a.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts_a.url_for_path("/data"), dl_path_a).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path_a) == "content from server A\n");
    }

    REQUIRE(cache->entry_count() >= 1);

    {
        httplib::client::downloader dl(ts_b.ioc_, ts_b.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts_b.url_for_path("/data"), dl_path_b).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path_b) == "content from server B\n");
    }

    REQUIRE(cache->entry_count() >= 2);

    fs::remove(server_path_a);
    fs::remove(server_path_b);
    fs::remove(dl_path_a);
    fs::remove(dl_path_b);
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
    ts.router().set_http_handler<httplib::method::get>("/bigcancel",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<httplib::method::head>("/bigcancel",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   { resp.set_file_content(server_path); });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);

    std::mutex cv_mtx;
    std::condition_variable cv;
    bool download_started = false;

    dl.set_state_callback(
        [&](httplib::client::downloader::state st, boost::system::error_code ec)
        {
            if (st == httplib::client::downloader::state::downloading)
            {
                {
                    std::lock_guard<std::mutex> lk(cv_mtx);
                    download_started = true;
                }
                cv.notify_one();
            }
        });

    std::thread cancel_thread(
        [&]()
        {
            std::unique_lock<std::mutex> lk(cv_mtx);
            cv.wait(lk, [&] { return download_started; });
            dl.cancel();
        });

    auto ec = dl.download(ts.url_for_path("/bigcancel"), dl_path).get();
    REQUIRE(ec);
    REQUIRE(dl.current_state() == httplib::client::downloader::state::cancelled);
    cancel_thread.join();

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path, rm_ec);
}

TEST_CASE("Downloader: download after cancel starts cleanly", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_recancel_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "after-cancel\n";
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_recancel_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/recancel",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<httplib::method::head>("/recancel",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::content_length, "13");
                                                       resp.set(httplib::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);

    // A cancel issued while idle applies to the interrupted run only. The next
    // download must start from a clean slate instead of failing once.
    dl.cancel();
    auto after_cancel = dl.download(ts.url_for_path("/recancel"), dl_path).get();
    REQUIRE_FALSE(after_cancel);
    REQUIRE(read_file(dl_path) == "after-cancel\n");

    // Cancelling a completed run must not poison the following run either.
    dl.cancel();
    auto again = dl.download(ts.url_for_path("/recancel"), dl_path).get();
    REQUIRE_FALSE(again);
    REQUIRE(read_file(dl_path) == "after-cancel\n");

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: cancel at connect aborts a would-be cache hit", "[downloader]")
{
    // Regression: a fresh run resets its cancellation state while starting, so a
    // cancel that lands right as the run begins used to be lost -- a fresh cache
    // hit would still copy the body and report success after the caller had
    // cancelled. Triggering the cancel from the "connecting" state callback pins
    // that start-of-run window deterministically.
    auto server_path = fs::temp_directory_path() / "httplib_dl_startcancel_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "start-cancel content\n";
    }

    auto dl_path1 = fs::temp_directory_path() / "httplib_dl_startcancel_out1.bin";
    auto dl_path2 = fs::temp_directory_path() / "httplib_dl_startcancel_out2.bin";
    auto cache_dir = fs::temp_directory_path() / "httplib_dl_startcancel_cache";
    fs::remove_all(cache_dir);

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/startcancel",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set(httplib::field::etag, "\"sc1\"");
                                                      resp.set(httplib::field::cache_control, "max-age=3600");
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/startcancel",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::content_length, "21");
                                                       resp.set(httplib::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);

    // Prime the cache with a fresh entry so the next run can be served without I/O.
    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/startcancel"), dl_path1).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path1) == "start-cancel content\n");
    }
    REQUIRE(cache->entry_count() >= 1);

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        dl.set_state_callback(
            [&](httplib::client::downloader::state st, boost::system::error_code)
            {
                if (st == httplib::client::downloader::state::connecting)
                {
                    dl.cancel();
                }
            });
        auto ec = dl.download(ts.url_for_path("/startcancel"), dl_path2).get();
        REQUIRE(ec);
        REQUIRE(dl.current_state() == httplib::client::downloader::state::cancelled);
    }
    REQUIRE_FALSE(fs::exists(dl_path2));

    fs::remove(server_path);
    fs::remove(dl_path1);
    fs::remove_all(cache_dir);
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
    ts.router().set_http_handler<httplib::method::get>("/retry-me",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      int a = attempt.fetch_add(1);
                                                      if (a < 2)
                                                      {
                                                          resp.set_empty_content(httplib::status::internal_server_error);
                                                      }
                                                      else
                                                      {
                                                          resp.set_file_content(server_path);
                                                      }
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/retry-me",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::content_length, "9");
                                                       resp.set(httplib::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .max_retries = 3 });
    auto ec = dl.download(ts.url_for_path("/retry-me"), dl_path).get();
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "retry-ok\n");
    REQUIRE(attempt.load() == 3);

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Downloader: download without content-length", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_nocl_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "no content-length\n";
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_nocl_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/no-cl",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    auto ec = dl.download(ts.url_for_path("/no-cl"), dl_path).get();
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

    auto cfg = dl.get_config();
    REQUIRE(cfg.max_retries == 3);
    REQUIRE(cfg.resume == true);
    REQUIRE(cfg.verify_ssl == true);
    REQUIRE(cfg.max_speed_bytes_per_sec == 0);
    REQUIRE(cfg.acquire_timeout == std::chrono::seconds(30));
    REQUIRE(cfg.retry_backoff == std::chrono::milliseconds(200));
    REQUIRE(dl.get_cache() == nullptr);

    dl.set_config({ .max_retries = 5, .resume = false });
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
    ts.router().set_http_handler<httplib::method::get>("/auth",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      auto auth = req[httplib::field::authorization];
                                                      if (auth != "Bearer secret-token")
                                                      {
                                                          resp.set_empty_content(httplib::status::forbidden);
                                                          return;
                                                      }
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/auth",
                                                   [&](httplib::server::request& req, httplib::server::response& resp)
                                                   {
                                                       auto auth = req[httplib::field::authorization];
                                                       if (auth != "Bearer secret-token")
                                                       {
                                                           resp.set_empty_content(httplib::status::forbidden);
                                                           return;
                                                       }
                                                       resp.set(httplib::field::content_length, "13");
                                                       resp.set(httplib::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    httplib::headers headers;
    headers.set(httplib::field::authorization, "Bearer secret-token");
    auto ec = dl.download(ts.url_for_path("/auth"), dl_path, headers).get();
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "with-headers\n");

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("http_client_pool: acquire timeout", "[downloader]")
{
    dl_test_scaffold ts;
    ts.start();

    auto pool = std::make_shared<httplib::client::http_client_pool>(ts.ioc_.get_executor(),
                                                                    httplib::client::pool_params { .max_size = 1 });

    auto h1 = co_spawn(ts.ioc_, pool->async_acquire("127.0.0.1", ts.endpoint.port(), httplib::url::scheme::plain), net::use_future).get();
    REQUIRE(h1);

    auto t0 = std::chrono::steady_clock::now();
    auto h2 = co_spawn(ts.ioc_,
                       pool->async_acquire("127.0.0.1", ts.endpoint.port(), httplib::url::scheme::plain, std::chrono::milliseconds(200)),
                       net::use_future)
                  .get();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);

    REQUIRE(h2.has_error());
    REQUIRE(h2.error() == boost::system::errc::timed_out);
    REQUIRE(elapsed >= std::chrono::milliseconds(100));
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

    // The cache stores the metadata blob verbatim; it never parses it.
    cache.put("http://example.com/test.txt", src, "etag=abc;content_type=text/plain");

    auto entry = cache.get("http://example.com/test.txt");
    REQUIRE(entry.has_value());
    REQUIRE(entry->body_size > 0);
    REQUIRE(entry->metadata == "etag=abc;content_type=text/plain");
    REQUIRE(read_file(entry->body_path) == "hello cache\n");

    auto miss = cache.get("http://example.com/other.txt");
    REQUIRE_FALSE(miss.has_value());

    cache.remove("http://example.com/test.txt");
    REQUIRE_FALSE(cache.get("http://example.com/test.txt").has_value());

    cache.put("http://a.com/1", src, "m");
    cache.put("http://a.com/2", src, "m");
    REQUIRE(cache.get("http://a.com/1").has_value());
    REQUIRE(cache.get("http://a.com/2").has_value());
    REQUIRE(cache.entry_count() == 2);

    cache.clear();
    REQUIRE_FALSE(cache.get("http://a.com/1").has_value());
    REQUIRE(cache.entry_count() == 0);

    fs::remove_all(tmp);
}

TEST_CASE("disk_cache: update_metadata keeps the body", "[downloader]")
{
    auto tmp = fs::temp_directory_path() / "httplib_dl_cache_updmeta";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    auto src = tmp / "src.bin";
    {
        std::ofstream f(src, std::ios::binary);
        std::string data = "body stays\n";
        f.write(data.data(), data.size());
    }

    httplib::client::disk_cache cache(tmp);
    cache.put("http://example.com/m", src, "v1");

    auto before = cache.get("http://example.com/m");
    REQUIRE(before.has_value());
    REQUIRE(before->metadata == "v1");

    auto future = std::chrono::system_clock::now() + std::chrono::hours(1);
    REQUIRE(cache.update_metadata("http://example.com/m", "v2", future));

    auto after = cache.get("http://example.com/m");
    REQUIRE(after.has_value());
    REQUIRE(after->metadata == "v2");
    REQUIRE(after->expires_at.has_value());
    REQUIRE(after->body_size == before->body_size);
    REQUIRE(read_file(after->body_path) == "body stays\n");

    // update_metadata on a missing key is a no-op that reports failure.
    REQUIRE_FALSE(cache.update_metadata("http://example.com/missing", "x", std::nullopt));

    fs::remove_all(tmp);
}

TEST_CASE("disk_cache: entry from another format version is treated as a miss", "[downloader]")
{
    auto tmp = fs::temp_directory_path() / "httplib_dl_cache_version";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    auto src = tmp / "src.bin";
    {
        std::ofstream f(src, std::ios::binary);
        std::string data = "versioned payload\n";
        f.write(data.data(), data.size());
    }

    httplib::client::disk_cache cache(tmp);
    cache.put("http://example.com/v", src, "m");

    auto entry = cache.get("http://example.com/v");
    REQUIRE(entry.has_value());

    auto edir = entry->body_path.parent_path();
    {
        std::ifstream f(edir / "version", std::ios::binary);
        std::string v;
        f >> v;
        REQUIRE_FALSE(v.empty());
    }

    // Roll the entry back to an unknown layout version: it must never be
    // served, and get() must evict it.
    {
        std::ofstream f(edir / "version", std::ios::binary | std::ios::trunc);
        f << "0";
    }
    REQUIRE_FALSE(cache.get("http://example.com/v").has_value());
    REQUIRE_FALSE(fs::exists(edir));

    fs::remove_all(tmp);
}

TEST_CASE("disk_cache: missing source does not clobber existing entry", "[downloader]")
{
    auto tmp = fs::temp_directory_path() / "httplib_dl_cache_preserve";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    auto src = tmp / "src.bin";
    {
        std::ofstream f(src, std::ios::binary);
        std::string data = "original payload\n";
        f.write(data.data(), data.size());
    }

    httplib::client::disk_cache cache(tmp);

    cache.put("http://example.com/x", src, "etag=orig");

    auto before = cache.get("http://example.com/x");
    REQUIRE(before.has_value());
    auto size_before = before->body_size;
    REQUIRE(size_before > 0);

    // A put whose source vanished must leave the existing entry untouched
    // instead of deleting it.
    cache.put("http://example.com/x", tmp / "does-not-exist.bin", "etag=orig");

    auto after = cache.get("http://example.com/x");
    REQUIRE(after.has_value());
    REQUIRE(after->body_size == size_before);
    REQUIRE(read_file(after->body_path) == "original payload\n");

    fs::remove_all(tmp);
}

TEST_CASE("disk_cache: expired entry is evicted on get without deadlock", "[downloader]")
{
    auto tmp = fs::temp_directory_path() / "httplib_dl_cache_expiry";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    auto src = tmp / "src.bin";
    {
        std::ofstream f(src, std::ios::binary);
        std::string data = "aged payload\n";
        f.write(data.data(), data.size());
    }

    httplib::client::disk_cache cache(tmp);
    cache.set_max_age(std::chrono::seconds(1));

    cache.put("http://example.com/aged", src, "content_type=text/plain");

    auto entry = cache.get("http://example.com/aged");
    REQUIRE(entry.has_value());

    // Backdate the body so it exceeds max_age, then get() must evict it and
    // return nullopt (regression: it used to call remove() under the lock and
    // self-deadlock on the non-recursive mutex).
    std::error_code ec;
    fs::last_write_time(entry->body_path, fs::file_time_type::clock::now() - std::chrono::hours(1), ec);
    REQUIRE_FALSE(ec);

    auto expired = cache.get("http://example.com/aged");
    REQUIRE_FALSE(expired.has_value());

    fs::remove_all(tmp);
}

TEST_CASE("Downloader: cache is isolated by request credentials", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_authcache_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "shared payload\n";
    }

    auto dl_path_a = fs::temp_directory_path() / "httplib_dl_authcache_a.bin";
    auto dl_path_b = fs::temp_directory_path() / "httplib_dl_authcache_b.bin";
    auto cache_dir = fs::temp_directory_path() / "httplib_dl_authcache_dir";
    fs::remove_all(cache_dir);

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/secret",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      auto auth = std::string(req[httplib::field::authorization]);
                                                      resp.set(httplib::field::etag, "\"" + auth + "\"");
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/secret",
                                                   [&](httplib::server::request& req, httplib::server::response& resp)
                                                   {
                                                       auto auth = std::string(req[httplib::field::authorization]);
                                                       resp.set(httplib::field::etag, "\"" + auth + "\"");
                                                       resp.set_file_content(server_path);
                                                   });
    ts.start();

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);

    httplib::headers headers_a;
    headers_a.set(httplib::field::authorization, "Bearer user-a");
    httplib::headers headers_b;
    headers_b.set(httplib::field::authorization, "Bearer user-b");

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/secret"), dl_path_a, headers_a).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path_a) == "shared payload\n");
    }
    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/secret"), dl_path_b, headers_b).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path_b) == "shared payload\n");
    }

    // Different credentials must never share a single cache entry.
    REQUIRE(cache->entry_count() >= 2);

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path_a, rm_ec);
    fs::remove(dl_path_b, rm_ec);
    fs::remove_all(cache_dir, rm_ec);
}

TEST_CASE("Downloader: no-store responses are not cached", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_nostore_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "no-store payload\n";
    }

    auto dl_path = fs::temp_directory_path() / "httplib_dl_nostore_out.bin";
    auto cache_dir = fs::temp_directory_path() / "httplib_dl_nostore_dir";
    fs::remove_all(cache_dir);

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/nostore",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set(httplib::field::cache_control, "no-store");
                                                      resp.set(httplib::field::etag, "\"ns\"");
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/nostore",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   { resp.set_file_content(server_path); });
    ts.start();

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_cache(cache);
    auto ec = dl.download(ts.url_for_path("/nostore"), dl_path).get();
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "no-store payload\n");

    REQUIRE(cache->entry_count() == 0);

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path, rm_ec);
    fs::remove_all(cache_dir, rm_ec);
}

TEST_CASE("Downloader: fresh cache entry is served without network", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_fresh_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "fresh payload\n";
    }

    auto dl_path1 = fs::temp_directory_path() / "httplib_dl_fresh_out1.bin";
    auto dl_path2 = fs::temp_directory_path() / "httplib_dl_fresh_out2.bin";
    auto cache_dir = fs::temp_directory_path() / "httplib_dl_fresh_dir";
    fs::remove_all(cache_dir);

    std::atomic<int> get_hits { 0 };
    std::atomic<int> head_hits { 0 };

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/fresh",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      get_hits.fetch_add(1);
                                                      resp.set(httplib::field::etag, "\"f1\"");
                                                      resp.set(httplib::field::cache_control, "max-age=3600");
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/fresh",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       head_hits.fetch_add(1);
                                                       resp.set(httplib::field::etag, "\"f1\"");
                                                       resp.set(httplib::field::cache_control, "max-age=3600");
                                                       resp.set_file_content(server_path);
                                                   });
    ts.start();

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/fresh"), dl_path1).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path1) == "fresh payload\n");
    }

    auto gets_after_first = get_hits.load();
    auto heads_after_first = head_hits.load();
    REQUIRE(gets_after_first == 1);

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/fresh"), dl_path2).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path2) == "fresh payload\n");
    }

    // The second download is still fresh, so it must not touch the network.
    REQUIRE(get_hits.load() == gets_after_first);
    REQUIRE(head_hits.load() == heads_after_first);

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path1, rm_ec);
    fs::remove(dl_path2, rm_ec);
    fs::remove_all(cache_dir, rm_ec);
}

TEST_CASE("Downloader: stale entry is revalidated with 304 and keeps its body", "[downloader]")
{
    auto dl_path1 = fs::temp_directory_path() / "httplib_dl_reval_out1.bin";
    auto dl_path2 = fs::temp_directory_path() / "httplib_dl_reval_out2.bin";
    auto cache_dir = fs::temp_directory_path() / "httplib_dl_reval_dir";
    fs::remove_all(cache_dir);

    std::atomic<int> get_hits { 0 };
    std::atomic<int> body_gets { 0 };

    std::string const payload = "revalidated payload\n";

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/reval",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      get_hits.fetch_add(1);
                                                      if (req[httplib::field::if_none_match] == "\"r1\"")
                                                      {
                                                          resp.set_empty_content(httplib::status::not_modified);
                                                          return;
                                                      }
                                                      body_gets.fetch_add(1);
                                                      resp.set(httplib::field::etag, "\"r1\"");
                                                      resp.set(httplib::field::cache_control, "no-cache");
                                                      resp.set_string_content(payload, "text/plain");
                                                  });
    ts.start();

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/reval"), dl_path1).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path1) == "revalidated payload\n");
    }

    auto gets_after_first = get_hits.load();
    auto bodies_after_first = body_gets.load();
    REQUIRE(gets_after_first == 1);
    REQUIRE(bodies_after_first == 1);

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/reval"), dl_path2).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path2) == "revalidated payload\n");
    }

    // no-cache forces a conditional GET; the 304 must reuse the cached body
    // without a second body-bearing GET.
    REQUIRE(get_hits.load() == gets_after_first + 1);
    REQUIRE(body_gets.load() == bodies_after_first);

    std::error_code rm_ec;
    fs::remove(dl_path1, rm_ec);
    fs::remove(dl_path2, rm_ec);
    fs::remove_all(cache_dir, rm_ec);
}

// ===========================================================================
// additional downloader tests (plan §24)
// ===========================================================================

TEST_CASE("Downloader: cancel while paused aborts immediately", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_pcancel_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }
    auto dl_path = fs::temp_directory_path() / "httplib_dl_pcancel_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/pcancel",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      std::this_thread::sleep_for(std::chrono::milliseconds(300));
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/pcancel",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   { resp.set_file_content(server_path); });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);

    std::mutex cv_mtx;
    std::condition_variable cv;
    std::atomic<bool> paused_seen { false };

    dl.set_state_callback(
        [&](httplib::client::downloader::state st, boost::system::error_code ec)
        {
            if (st == httplib::client::downloader::state::downloading)
            {
                dl.pause();
            }
            if (st == httplib::client::downloader::state::paused)
            {
                paused_seen.store(true);
                cv.notify_all();
            }
        });

    std::thread control_thread(
        [&]()
        {
            std::unique_lock<std::mutex> lk(cv_mtx);
            cv.wait(lk, [&] { return paused_seen.load(); });
            dl.cancel();
        });

    // A pause can land before the state callback observes it; cancel() must
    // also wake a not-yet-notified paused coroutine, so issue it from a timer
    // thread as well in case the transfer finished already.
    auto ec = dl.download(ts.url_for_path("/pcancel"), dl_path).get();
    control_thread.join();

    REQUIRE(ec);
    REQUIRE(ec == boost::asio::error::operation_aborted);
    REQUIRE(dl.current_state() == httplib::client::downloader::state::cancelled);

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path, rm_ec);
}

TEST_CASE("Downloader: persistent server 500 ends failed", "[downloader]")
{
    auto dl_path = fs::temp_directory_path() / "httplib_dl_500_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/500",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_empty_content(httplib::status::internal_server_error); });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .max_retries = 1 });

    auto ec = dl.download(ts.url_for_path("/500"), dl_path).get();
    REQUIRE(ec);
    REQUIRE(dl.current_state() == httplib::client::downloader::state::failed);
    REQUIRE_FALSE(fs::exists(dl_path));

    std::error_code rm_ec;
    fs::remove(dl_path, rm_ec);
}

TEST_CASE("Downloader: invalid URL maps to a failed error, not a throw", "[downloader]")
{
    auto dl_path = fs::temp_directory_path() / "httplib_dl_badurl_out.bin";

    dl_test_scaffold ts;
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);

    auto ec = dl.download("not a url at all", dl_path).get();
    REQUIRE(ec);
    REQUIRE(ec == boost::system::errc::make_error_code(boost::system::errc::invalid_argument));
    REQUIRE(dl.current_state() == httplib::client::downloader::state::failed);
    REQUIRE_FALSE(fs::exists(dl_path));

    fs::remove(dl_path);
}

TEST_CASE("Downloader: max speed throttles the transfer", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_speed_srv.bin";
    constexpr std::size_t kSize = 256 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }
    auto dl_path = fs::temp_directory_path() / "httplib_dl_speed_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/speed",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<httplib::method::head>("/speed",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   { resp.set_file_content(server_path); });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .max_speed_bytes_per_sec = 128 * 1024 });

    auto t0 = std::chrono::steady_clock::now();
    auto ec = dl.download(ts.url_for_path("/speed"), dl_path).get();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);

    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == read_file(server_path));
    // The Beast rate policy grants one window immediately and refills per second,
    // so 256 KiB at 128 KiB/s needs a second window (~1s). Without throttling the
    // local transfer would finish in a few milliseconds.
    REQUIRE(elapsed >= std::chrono::milliseconds(800));

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path, rm_ec);
}

TEST_CASE("Downloader: relative redirect Location is resolved", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_relredir_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "relative-ok\n";
    }
    auto dl_path = fs::temp_directory_path() / "httplib_dl_relredir_out.bin";

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/start",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_redirect("final", httplib::status::found); });
    ts.router().set_http_handler<httplib::method::get>("/final",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  { resp.set_file_content(server_path); });
    ts.router().set_http_handler<httplib::method::head>("/start",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::location, "final");
                                                       resp.set_empty_content(httplib::status::found);
                                                   });
    ts.router().set_http_handler<httplib::method::head>("/final",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   { resp.set_file_content(server_path); });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .max_redirects = 5 });
    auto ec = dl.download(ts.url_for_path("/start"), dl_path).get();
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "relative-ok\n");

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path, rm_ec);
}

TEST_CASE("Downloader: redirect response body does not corrupt connection reuse", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_redirbody_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "redirect-body-ok\n";
    }
    auto dl_path = fs::temp_directory_path() / "httplib_dl_redirbody_out.bin";

    // A redirect body large enough that it is buffered together with the
    // response header. If the downloader returns the connection to the pool
    // without draining it, the next request on that connection parses these
    // bytes as a response and fails.
    std::string const redirect_body(8192, 'X');

    // Track the client's ephemeral port for the GET /start and the redirected
    // GET /final. When the redirect body is drained the same pooled connection
    // is reused, so both land on the same port. If it is not drained the
    // response destructor closes the connection and the redirect uses a fresh
    // one (different port).
    std::atomic<std::uint16_t> start_port { 0 };
    std::atomic<std::uint16_t> final_port { 0 };

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>(
        "/start",
        [&](httplib::server::request& req, httplib::server::response& resp)
        {
            start_port.store(req.remote_endpoint().port());
            resp.set(httplib::field::location, "/final");
            resp.set_string_content(redirect_body, "text/plain", httplib::status::found);
        });
    ts.router().set_http_handler<httplib::method::get>("/final",
                                                  [&](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      final_port.store(req.remote_endpoint().port());
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/start",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::location, "/final");
                                                       resp.set_empty_content(httplib::status::found);
                                                   });
    ts.router().set_http_handler<httplib::method::head>("/final",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::content_length, "18");
                                                       resp.set(httplib::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .max_retries = 0, .max_redirects = 5 });
    auto ec = dl.download(ts.url_for_path("/start"), dl_path).get();
    REQUIRE_FALSE(ec);
    REQUIRE(read_file(dl_path) == "redirect-body-ok\n");
    REQUIRE(start_port.load() != 0);
    REQUIRE(final_port.load() == start_port.load());

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path, rm_ec);
}

TEST_CASE("Downloader: connection failure surfaces its real error code", "[downloader]")
{
    dl_test_scaffold ts;
    ts.start();

    // Find a port that is guaranteed to have no listener.
    net::ip::tcp::acceptor probe(ts.ioc_);
    probe.open(net::ip::tcp::v4());
    probe.bind({ net::ip::make_address("127.0.0.1"), 0 });
    probe.listen(1);
    auto closed_port = probe.local_endpoint().port();
    probe.close();

    auto dl_path = fs::temp_directory_path() / "httplib_dl_connfail_out.bin";

    httplib::client::downloader dl(ts.ioc_, ts.pool);
    dl.set_config({ .max_retries = 0, .retry_backoff = std::chrono::milliseconds(0) });
    auto ec = dl.download(std::format("http://127.0.0.1:{}/nope", closed_port), dl_path).get();

    REQUIRE(ec);
    // Previously every transport failure was folded into a generic timeout,
    // hiding the true cause.
    REQUIRE(ec != boost::system::errc::make_error_code(boost::system::errc::timed_out));
    REQUIRE(dl.current_state() == httplib::client::downloader::state::failed);

    std::error_code rm_ec;
    fs::remove(dl_path, rm_ec);
}

TEST_CASE("Downloader: cache hit emits a final progress tick", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_cacheprog_srv.txt";
    std::string const payload = "cached-progress\n";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << payload;
    }

    auto dl_path1 = fs::temp_directory_path() / "httplib_dl_cacheprog_out1.bin";
    auto dl_path2 = fs::temp_directory_path() / "httplib_dl_cacheprog_out2.bin";
    auto cache_dir = fs::temp_directory_path() / "httplib_dl_cacheprog_dir";
    fs::remove_all(cache_dir);

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/cacheprog",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set(httplib::field::etag, "\"cp\"");
                                                      resp.set(httplib::field::cache_control, "max-age=3600");
                                                      resp.set_file_content(server_path);
                                                  });
    ts.router().set_http_handler<httplib::method::head>("/cacheprog",
                                                   [&](httplib::server::request&, httplib::server::response& resp)
                                                   {
                                                       resp.set(httplib::field::etag, "\"cp\"");
                                                       resp.set(httplib::field::cache_control, "max-age=3600");
                                                       resp.set(httplib::field::content_length,
                                                                std::to_string(payload.size()));
                                                       resp.set(httplib::field::accept_ranges, "bytes");
                                                   });
    ts.start();

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        auto ec = dl.download(ts.url_for_path("/cacheprog"), dl_path1).get();
        REQUIRE_FALSE(ec);
    }

    // The second download is served from a still-fresh cache entry; it must
    // still report 100% progress so observers reach a terminal update.
    std::atomic<std::uint64_t> last_downloaded { 0 };
    std::atomic<std::uint64_t> last_total { 0 };
    std::atomic<int> progress_ticks { 0 };
    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        dl.set_progress_callback(
            [&](httplib::client::downloader::progress_info const& info)
            {
                ++progress_ticks;
                last_downloaded.store(info.downloaded_bytes);
                last_total.store(info.total_bytes);
            });
        auto ec = dl.download(ts.url_for_path("/cacheprog"), dl_path2).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path2) == payload);
    }

    REQUIRE(progress_ticks.load() >= 1);
    REQUIRE(last_downloaded.load() == payload.size());
    REQUIRE(last_total.load() == payload.size());

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path1, rm_ec);
    fs::remove(dl_path2, rm_ec);
    fs::remove_all(cache_dir, rm_ec);
}

TEST_CASE("Downloader: cache entry revalidates through a redirect", "[downloader]")
{
    auto server_path = fs::temp_directory_path() / "httplib_dl_redircache_srv.txt";
    std::string const payload = "redirect cache payload\n";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << payload;
    }

    auto dl_path1 = fs::temp_directory_path() / "httplib_dl_redircache_out1.bin";
    auto dl_path2 = fs::temp_directory_path() / "httplib_dl_redircache_out2.bin";
    auto cache_dir = fs::temp_directory_path() / "httplib_dl_redircache_dir";
    fs::remove_all(cache_dir);

    std::atomic<int> start_gets { 0 };
    std::atomic<int> final_gets { 0 };
    std::atomic<int> final_bodies { 0 };
    std::atomic<bool> final_saw_conditional { false };

    dl_test_scaffold ts;
    ts.router().set_http_handler<httplib::method::get>("/start",
                                                      [&](httplib::server::request&, httplib::server::response& resp)
                                                      {
                                                          start_gets.fetch_add(1);
                                                          resp.set_redirect("/final", httplib::status::found);
                                                      });
    ts.router().set_http_handler<httplib::method::get>("/final",
                                                      [&](httplib::server::request& req,
                                                          httplib::server::response& resp)
                                                      {
                                                          final_gets.fetch_add(1);
                                                          if (req[httplib::field::if_none_match] == "\"rc1\"")
                                                          {
                                                              final_saw_conditional.store(true);
                                                              resp.set_empty_content(httplib::status::not_modified);
                                                              return;
                                                          }
                                                          final_bodies.fetch_add(1);
                                                          resp.set(httplib::field::etag, "\"rc1\"");
                                                          resp.set(httplib::field::cache_control, "no-cache");
                                                          resp.set_string_content(payload, "text/plain");
                                                      });
    ts.start();

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        dl.set_config({ .max_redirects = 5 });
        auto ec = dl.download(ts.url_for_path("/start"), dl_path1).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path1) == payload);
    }

    REQUIRE(start_gets.load() == 1);
    REQUIRE(final_gets.load() == 1);
    REQUIRE(final_bodies.load() == 1);

    {
        httplib::client::downloader dl(ts.ioc_, ts.pool);
        dl.set_cache(cache);
        dl.set_config({ .max_redirects = 5 });
        auto ec = dl.download(ts.url_for_path("/start"), dl_path2).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path2) == payload);
    }

    // no-cache on the final response forces revalidation on the second run: the
    // conditional request must be re-sent through the redirect chain, answered
    // by a 304 at the final URL, and served from the cached body.
    REQUIRE(start_gets.load() == 2);
    REQUIRE(final_gets.load() == 2);
    REQUIRE(final_saw_conditional.load());
    REQUIRE(final_bodies.load() == 1);

    std::error_code rm_ec;
    fs::remove(server_path, rm_ec);
    fs::remove(dl_path1, rm_ec);
    fs::remove(dl_path2, rm_ec);
    fs::remove_all(cache_dir, rm_ec);
}

TEST_CASE("Downloader: revalidation must not leak validators to the redirect origin", "[downloader]")
{
    auto srv_path = fs::temp_directory_path() / "httplib_dl_crosscache_srv.txt";
    std::string const payload = "cross cache payload\n";
    {
        std::ofstream f(srv_path, std::ios::binary);
        f << payload;
    }

    auto dl_path1 = fs::temp_directory_path() / "httplib_dl_crosscache_out1.bin";
    auto dl_path2 = fs::temp_directory_path() / "httplib_dl_crosscache_out2.bin";
    auto cache_dir = fs::temp_directory_path() / "httplib_dl_crosscache_dir";
    fs::remove_all(cache_dir);

    net::io_context ioc;
    httplib::server::http_server target { ioc };
    httplib::server::http_server origin { ioc };
    std::thread worker;
    auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    target.set_logger(std::make_shared<spdlog::logger>("t", null_sink));
    origin.set_logger(std::make_shared<spdlog::logger>("o", null_sink));

    std::atomic<int> origin_sees_conditional { 0 };
    std::atomic<int> target_bodies { 0 };

    target.router().set_http_handler<httplib::method::get>(
        "/target",
        [&](httplib::server::request& req, httplib::server::response& resp)
        {
            if (req[httplib::field::if_none_match] == "\"x1\"")
            {
                resp.set_empty_content(httplib::status::not_modified);
                return;
            }
            target_bodies.fetch_add(1);
            resp.set(httplib::field::etag, "\"x1\"");
            resp.set(httplib::field::cache_control, "no-cache");
            resp.set_string_content(payload, "text/plain");
        });
    target.listen("127.0.0.1", 0);
    auto target_port = target.local_endpoint().port();

    origin.router().set_http_handler<httplib::method::get>(
        "/start",
        [&](httplib::server::request& req, httplib::server::response& resp)
        {
            if (req.has(httplib::field::if_none_match) || req.has(httplib::field::if_modified_since))
            {
                origin_sees_conditional.fetch_add(1);
            }
            resp.set_redirect(std::format("http://127.0.0.1:{}/target", target_port), httplib::status::found);
        });
    origin.listen("127.0.0.1", 0);

    auto pool = std::make_shared<httplib::client::http_client_pool>(ioc.get_executor(),
                                                                    httplib::client::pool_params { .max_size = 8 });
    target.run();
    origin.run();
    worker = std::thread([&] { ioc.run(); });

    auto cache = std::make_shared<httplib::client::disk_cache>(cache_dir);
    auto origin_url = std::format("http://127.0.0.1:{}/start", origin.local_endpoint().port());

    {
        httplib::client::downloader dl(ioc, pool);
        dl.set_cache(cache);
        dl.set_config({ .max_redirects = 5 });
        auto ec = dl.download(origin_url, dl_path1).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path1) == payload);
    }

    {
        httplib::client::downloader dl(ioc, pool);
        dl.set_cache(cache);
        dl.set_config({ .max_redirects = 5 });
        auto ec = dl.download(origin_url, dl_path2).get();
        REQUIRE_FALSE(ec);
        REQUIRE(read_file(dl_path2) == payload);
    }

    // The cached body came from the target's response, so its ETag/Last-Modified
    // belong to that origin. Revalidating re-follows the redirect, but the
    // validators are only ever attached to the hop whose cache they came from:
    // the redirecting origin sees none, and the target answers 304 so the body
    // is reused without a second download.
    REQUIRE(origin_sees_conditional.load() == 0);
    REQUIRE(target_bodies.load() == 1);

    pool->stop();
    origin.stop();
    target.stop();
    ioc.stop();
    worker.join();

    std::error_code rm_ec;
    fs::remove(srv_path, rm_ec);
    fs::remove(dl_path1, rm_ec);
    fs::remove(dl_path2, rm_ec);
    fs::remove_all(cache_dir, rm_ec);
}
