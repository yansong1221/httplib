#include "common.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/download_scheduler.hpp"
#include "httplib/client/downloader.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>

using namespace test_common;

namespace
{
    namespace fs = std::filesystem;

    using sched_ptr = std::shared_ptr<httplib::client::download_scheduler>;

    struct dl_sched_scaffold
    {
        net::io_context ioc_;
        std::thread worker_;
        httplib::server::http_server server { ioc_ };
        httplib::tcp::endpoint endpoint;
        bool started_ = false;
        std::shared_ptr<httplib::client::http_client_pool> pool;

        dl_sched_scaffold()
        {
            auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            server.set_logger(std::make_shared<spdlog::logger>("httplib.tests", null_sink));
            pool = std::make_shared<httplib::client::http_client_pool>(ioc_.get_executor(),
                                                                       httplib::client::pool_params { .max_size = 8 });
            pool->start();
        }

        ~dl_sched_scaffold()
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

    // Start async_run on the scheduler, keeping it alive via the shared_ptr held
    // by the detached coroutine.
    void
    start_scheduler(net::io_context& ioc, sched_ptr sched)
    {
        net::co_spawn(
            ioc,
            [sched]() -> net::awaitable<void> { co_await sched->async_run(); },
            net::detached);
    }

    // Gracefully stop the scheduler and block until fully drained.
    void
    shutdown_scheduler(net::io_context& ioc, sched_ptr sched)
    {
        std::promise<void> sp;
        auto sf = sp.get_future();
        net::co_spawn(
            ioc,
            [sched, &sp]() -> net::awaitable<void>
            {
                co_await sched->async_shutdown();
                sp.set_value();
            },
            net::detached);
        sf.get();
    }

    // Poll until `pred` becomes true.
    bool
    wait_until(std::function<bool()> pred, int timeout_ms = 10000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return pred();
    }

    bool
    is_terminal(httplib::client::downloader::state st)
    {
        return st == httplib::client::downloader::state::completed ||
               st == httplib::client::downloader::state::failed ||
               st == httplib::client::downloader::state::cancelled;
    }
} // namespace

TEST_CASE("Download scheduler: basic single task", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_basic_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "scheduler hello\n";
    }
    auto dl_path = fs::temp_directory_path() / "sched_basic_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/file",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/file",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "16");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);
    auto id = sched->add(ts.url_for_path("/file"), dl_path);
    start_scheduler(ts.ioc_, sched);
    REQUIRE(id > 0);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id).state); }));
    REQUIRE(sched->get_status(id).state == httplib::client::downloader::state::completed);
    REQUIRE(read_file(dl_path) == "scheduler hello\n");

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Download scheduler: max_concurrent limits tasks", "[download_scheduler]")
{
    // Each task is a big, reasonably slow transfer; max_concurrent=2 means at
    // most two can be running at any instant.
    auto server_path = fs::temp_directory_path() / "sched_conc_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    std::atomic<int> active { 0 };
    std::atomic<int> max_seen { 0 };

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            int cur = active.fetch_add(1) + 1;
            int prev = max_seen.load();
            while (cur > prev && !max_seen.compare_exchange_weak(prev, cur))
            {
            }
            resp.set_file_content(server_path);
            active.fetch_sub(1);
        });
    ts.router().set_http_handler<http::verb::head>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(
        ts.ioc_.get_executor(), ts.pool, httplib::client::download_scheduler::scheduler_config { .max_concurrent = 2 });

    std::vector<fs::path> outs;
    for (int i = 0; i < 3; ++i)
    {
        outs.push_back(fs::temp_directory_path() / std::format("sched_conc_out_{}.bin", i));
        sched->add(ts.url_for_path("/slow"), outs.back());
    }
    start_scheduler(ts.ioc_, sched);

    // Wait until all tasks reach a terminal state.
    REQUIRE(wait_until(
        [&]
        {
            for (auto const& s : sched->get_all_status())
            {
                if (!is_terminal(s.state))
                {
                    return false;
                }
            }
            return true;
        }));

    REQUIRE(max_seen.load() <= 2);

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    for (auto& p : outs)
    {
        fs::remove(p);
    }
}

TEST_CASE("Download scheduler: cancel all tasks", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_cancel_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/big",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            resp.set_file_content(server_path);
        });
    ts.router().set_http_handler<http::verb::head>(
        "/big",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);
    std::vector<fs::path> outs;
    for (int i = 0; i < 3; ++i)
    {
        outs.push_back(fs::temp_directory_path() / std::format("sched_cancel_out_{}.bin", i));
        sched->add(ts.url_for_path("/big"), outs.back());
    }
    start_scheduler(ts.ioc_, sched);

    // Wait until all three are running before cancelling.
    REQUIRE(wait_until([&] { return sched->active_count() >= 3; }));

    sched->cancel_all();

    // All tasks should settle into a terminal (cancelled) state.
    REQUIRE(wait_until(
        [&]
        {
            for (auto const& s : sched->get_all_status())
            {
                if (!is_terminal(s.state))
                {
                    return false;
                }
            }
            return true;
        }));

    for (auto const& s : sched->get_all_status())
    {
        REQUIRE(s.state == httplib::client::downloader::state::cancelled);
    }

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    for (auto& p : outs)
    {
        std::error_code ec;
        fs::remove(p, ec);
    }
}

TEST_CASE("Download scheduler: pause and resume", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_pause_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/p",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/p",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);
    auto dl_path = fs::temp_directory_path() / "sched_pause_out.bin";
    auto id = sched->add(ts.url_for_path("/p"), dl_path);
    start_scheduler(ts.ioc_, sched);

    // Wait until the transfer is in-flight.
    REQUIRE(wait_until(
        [&]
        {
            auto st = sched->get_status(id).state;
            return st == httplib::client::downloader::state::downloading ||
                   st == httplib::client::downloader::state::connecting;
        }));

    sched->pause(id);

    // It should reach paused state (or finish in the meantime).
    REQUIRE(wait_until(
        [&]
        {
            auto st = sched->get_status(id).state;
            return st == httplib::client::downloader::state::paused || is_terminal(st);
        }));

    if (sched->get_status(id).state == httplib::client::downloader::state::paused)
    {
        // Progress frozen while paused.
        auto frozen = sched->get_status(id);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(sched->get_status(id).downloaded_bytes == frozen.downloaded_bytes);

        sched->resume(id);
    }

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id).state); }));
    REQUIRE(sched->get_status(id).state == httplib::client::downloader::state::completed);

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Download scheduler: state callback fires", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_cb_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "cb test\n";
    }
    auto dl_path = fs::temp_directory_path() / "sched_cb_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/cb",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/cb",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "8");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);

    std::mutex mtx;
    bool saw_completed = false;
    std::atomic<int> cb_count { 0 };
    sched->set_state_callback(
        [&](httplib::client::download_scheduler::task_status const& s)
        {
            cb_count.fetch_add(1);
            std::lock_guard<std::mutex> lk(mtx);
            if (s.state == httplib::client::downloader::state::completed)
            {
                saw_completed = true;
            }
        });

    auto id = sched->add(ts.url_for_path("/cb"), dl_path);
    start_scheduler(ts.ioc_, sched);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id).state); }));
    REQUIRE(sched->get_status(id).state == httplib::client::downloader::state::completed);
    REQUIRE(read_file(dl_path) == "cb test\n");

    REQUIRE(wait_until([&] { return cb_count.load() > 0; }));
    {
        std::lock_guard<std::mutex> lk(mtx);
        REQUIRE(saw_completed);
    }

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Download scheduler: dynamic add while running", "[download_scheduler]")
{
    auto srv_a = fs::temp_directory_path() / "sched_dyn_a.txt";
    auto srv_b = fs::temp_directory_path() / "sched_dyn_b.txt";
    {
        std::ofstream fa(srv_a, std::ios::binary);
        fa << "file A\n";
        std::ofstream fb(srv_b, std::ios::binary);
        fb << "file B\n";
    }
    auto out_a = fs::temp_directory_path() / "sched_dyn_out_a.bin";
    auto out_b = fs::temp_directory_path() / "sched_dyn_out_b.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/a",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(srv_a); });
    ts.router().set_http_handler<http::verb::head>(
        "/a",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "7");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.router().set_http_handler<http::verb::get>(
        "/b",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(srv_b); });
    ts.router().set_http_handler<http::verb::head>(
        "/b",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "7");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);
    auto id_a = sched->add(ts.url_for_path("/a"), out_a);
    start_scheduler(ts.ioc_, sched);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto id_b = sched->add(ts.url_for_path("/b"), out_b);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id_a).state); }));
    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id_b).state); }));
    REQUIRE(sched->get_status(id_a).state == httplib::client::downloader::state::completed);
    REQUIRE(sched->get_status(id_b).state == httplib::client::downloader::state::completed);
    REQUIRE(read_file(out_a) == "file A\n");
    REQUIRE(read_file(out_b) == "file B\n");

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(srv_a);
    fs::remove(srv_b);
    fs::remove(out_a);
    fs::remove(out_b);
}

TEST_CASE("Download scheduler: pending task cancel before dispatch", "[download_scheduler]")
{
    // max_concurrent=1: only one task runs at a time, so the second stays
    // pending. Cancel it before it ever gets dispatched.
    auto server_path = fs::temp_directory_path() / "sched_pend_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            resp.set_file_content(server_path);
        });
    ts.router().set_http_handler<http::verb::head>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(
        ts.ioc_.get_executor(), ts.pool, httplib::client::download_scheduler::scheduler_config { .max_concurrent = 1 });

    auto out1 = fs::temp_directory_path() / "sched_pend_out1.bin";
    auto out2 = fs::temp_directory_path() / "sched_pend_out2.bin";

    auto id1 = sched->add(ts.url_for_path("/slow"), out1);
    auto id2 = sched->add(ts.url_for_path("/slow"), out2);
    start_scheduler(ts.ioc_, sched);

    // Wait until the second task is genuinely pending (never dispatched).
    REQUIRE(wait_until(
        [&]
        {
            auto st = sched->get_status(id2).state;
            return st == httplib::client::downloader::state::idle && sched->pending_count() > 0;
        },
        5000));

    sched->cancel(id2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto s2 = sched->get_status(id2);
    REQUIRE(s2.state == httplib::client::downloader::state::cancelled);

    // First task keeps going and completes normally.
    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id1).state); }, 15000));
    REQUIRE(sched->get_status(id1).state == httplib::client::downloader::state::completed);

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(out1);
    fs::remove(out2);
}

TEST_CASE("Download scheduler: shutdown drains running tasks", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_shutdown_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "shutdown ok\n";
    }
    auto dl_path = fs::temp_directory_path() / "sched_shutdown_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/done",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/done",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "12");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);
    sched->add(ts.url_for_path("/done"), dl_path);
    start_scheduler(ts.ioc_, sched);

    shutdown_scheduler(ts.ioc_, sched);

    // After shutdown, the scheduler should be quiescent.
    REQUIRE(sched->active_count() == 0);
    REQUIRE(sched->pending_count() == 0);

    fs::remove(server_path);
    fs::remove(dl_path);
}

// ===========================================================================
// additional scheduler tests (plan §24)
// ===========================================================================

TEST_CASE("Download scheduler: cancel a single running task", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_single_cancel_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    auto dl_path = fs::temp_directory_path() / "sched_single_cancel_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/big",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            resp.set_file_content(server_path);
        });
    ts.router().set_http_handler<http::verb::head>(
        "/big",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);
    auto id = sched->add(ts.url_for_path("/big"), dl_path);
    start_scheduler(ts.ioc_, sched);

    REQUIRE(wait_until([&] { return sched->active_count() >= 1; }));

    sched->cancel(id);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id).state); }, 15000));
    REQUIRE(sched->get_status(id).state == httplib::client::downloader::state::cancelled);

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    std::error_code ec;
    fs::remove(dl_path, ec);
    for (int i = 0; i < 8; ++i)
    {
        fs::remove(fs::path(dl_path.string() + ".part" + std::to_string(i)), ec);
    }
    fs::remove(fs::path(dl_path.string() + ".dlstate"), ec);
}

TEST_CASE("Download scheduler: progress callback carries id and url", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_prog_srv.bin";
    {
        std::ofstream f(server_path, std::ios::binary);
        for (int i = 0; i < 256 * 1024; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }
    auto dl_path = fs::temp_directory_path() / "sched_prog_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/prog",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/prog",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "262144");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);

    std::mutex mtx;
    std::vector<httplib::client::download_scheduler::task_id> cb_ids;
    std::string cb_url;
    std::uint64_t final_downloaded = 0;
    sched->set_progress_callback(
        [&](httplib::client::download_scheduler::task_status const& s)
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (cb_ids.empty())
            {
                cb_url = s.url;
            }
            cb_ids.push_back(s.id);
            final_downloaded = s.downloaded_bytes;
        });

    auto url = ts.url_for_path("/prog");
    auto id = sched->add(url, dl_path);
    start_scheduler(ts.ioc_, sched);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id).state); }));
    REQUIRE(sched->get_status(id).state == httplib::client::downloader::state::completed);

    REQUIRE(wait_until(
        [&]
        {
            std::lock_guard<std::mutex> lk(mtx);
            return !cb_ids.empty();
        }));
    {
        std::lock_guard<std::mutex> lk(mtx);
        REQUIRE(cb_url == url);
        for (auto cid : cb_ids)
        {
            REQUIRE(cid == id);
        }
        REQUIRE(final_downloaded == 262144);
    }

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Download scheduler: empty run exits immediately", "[download_scheduler]")
{
    dl_sched_scaffold ts;
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);

    std::promise<void> done;
    auto f = done.get_future();
    net::co_spawn(
        ts.ioc_,
        [sched, &done]() -> net::awaitable<void>
        {
            co_await sched->async_run();
            done.set_value();
        },
        net::detached);

    REQUIRE(f.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
}

TEST_CASE("Download scheduler: pending pause blocks dispatch until resume", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_ppause_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            resp.set_file_content(server_path);
        });
    ts.router().set_http_handler<http::verb::head>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(
        ts.ioc_.get_executor(), ts.pool, httplib::client::download_scheduler::scheduler_config { .max_concurrent = 1 });

    auto out1 = fs::temp_directory_path() / "sched_ppause_out1.bin";
    auto out2 = fs::temp_directory_path() / "sched_ppause_out2.bin";
    auto id1 = sched->add(ts.url_for_path("/slow"), out1);
    auto id2 = sched->add(ts.url_for_path("/slow"), out2);
    start_scheduler(ts.ioc_, sched);

    // The second task is genuinely pending.
    REQUIRE(wait_until([&] { return sched->pending_count() > 0; }));

    sched->pause(id2);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Paused-pending tasks must not be dispatched and stay not-terminal.
    REQUIRE(sched->get_status(id2).state == httplib::client::downloader::state::idle);
    REQUIRE_FALSE(is_terminal(sched->get_status(id2).state));
    REQUIRE(sched->active_count() == 1);

    sched->resume(id2);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id2).state); }, 15000));
    REQUIRE(sched->get_status(id2).state == httplib::client::downloader::state::completed);

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(out1);
    fs::remove(out2);
}

TEST_CASE("Download scheduler: cancel a paused-pending task", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_ppcancel_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            resp.set_file_content(server_path);
        });
    ts.router().set_http_handler<http::verb::head>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(
        ts.ioc_.get_executor(), ts.pool, httplib::client::download_scheduler::scheduler_config { .max_concurrent = 1 });

    auto out1 = fs::temp_directory_path() / "sched_ppcancel_out1.bin";
    auto out2 = fs::temp_directory_path() / "sched_ppcancel_out2.bin";
    auto id1 = sched->add(ts.url_for_path("/slow"), out1);
    auto id2 = sched->add(ts.url_for_path("/slow"), out2);
    start_scheduler(ts.ioc_, sched);

    REQUIRE(wait_until([&] { return sched->pending_count() > 0; }));

    sched->pause(id2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sched->cancel(id2);

    // The paused-pending task settles into cancelled without ever starting.
    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id2).state); }, 5000));
    REQUIRE(sched->get_status(id2).state == httplib::client::downloader::state::cancelled);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id1).state); }, 15000));

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(out1);
    fs::remove(out2);
}

TEST_CASE("Download scheduler: async_wait_any consumes one completion each", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_wany_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "wait-any\n";
    }

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/w",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/w",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "9");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);

    std::vector<httplib::client::download_scheduler::task_id> ids;
    for (int i = 0; i < 3; ++i)
    {
        auto out = fs::temp_directory_path() / std::format("sched_wany_out_{}.bin", i);
        ids.push_back(sched->add(ts.url_for_path("/w"), out));
    }
    start_scheduler(ts.ioc_, sched);

    std::vector<httplib::client::download_scheduler::task_id> got;
    for (int i = 0; i < 3; ++i)
    {
        std::promise<httplib::client::download_scheduler::task_id> p;
        auto f = p.get_future();
        net::co_spawn(
            ts.ioc_,
            [sched, &p]() -> net::awaitable<void>
            {
                auto st = co_await sched->async_wait_any();
                p.set_value(st.id);
            },
            net::detached);
        got.push_back(f.get());
    }

    // Each terminal task is consumed exactly once.
    std::sort(got.begin(), got.end());
    auto expect = ids;
    std::sort(expect.begin(), expect.end());
    REQUIRE(got == expect);

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    for (int i = 0; i < 3; ++i)
    {
        fs::remove(fs::temp_directory_path() / std::format("sched_wany_out_{}.bin", i));
    }
}

TEST_CASE("Download scheduler: async_wait_one returns immediately for done/missing tasks", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_wone_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "wait-one\n";
    }
    auto dl_path = fs::temp_directory_path() / "sched_wone_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/o",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/o",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "9");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);

    // Not-found: a typed task-not-found error.  (Does not need the driver.)
    {
        std::promise<httplib::client::download_scheduler::task_status> p;
        auto f = p.get_future();
        net::co_spawn(
            ts.ioc_,
            [sched, &p]() -> net::awaitable<void>
            {
                auto st = co_await sched->async_wait_one(std::numeric_limits<std::uint64_t>::max());
                p.set_value(st);
            },
            net::detached);
        auto st = f.get();
        REQUIRE(st.id == std::numeric_limits<std::uint64_t>::max());
        REQUIRE(st.state == httplib::client::downloader::state::cancelled);
        REQUIRE(st.error == boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory));
        REQUIRE(st.message == "task not found");
    }

    // Submit the task before starting the driver so async_run does not see an
    // empty task set and exit immediately (plan §24: empty run ends at once).
    auto id = sched->add(ts.url_for_path("/o"), dl_path);
    start_scheduler(ts.ioc_, sched);

    // Wait for the real task to finish first, then the waiter must return at once.
    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id).state); }));

    auto begin = std::chrono::steady_clock::now();
    std::promise<httplib::client::download_scheduler::task_status> p;
    auto f = p.get_future();
    net::co_spawn(
        ts.ioc_,
        [sched, id, &p]() -> net::awaitable<void>
        {
            auto st = co_await sched->async_wait_one(id);
            p.set_value(st);
        },
        net::detached);
    auto st = f.get();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
    REQUIRE(st.id == id);
    REQUIRE(st.state == httplib::client::downloader::state::completed);
    REQUIRE(elapsed < std::chrono::seconds(2));

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Download scheduler: shutdown while a task is paused", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_spause_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }
    auto dl_path = fs::temp_directory_path() / "sched_spause_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/p",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/p",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

        auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);
    auto id = sched->add(ts.url_for_path("/p"), dl_path);
    start_scheduler(ts.ioc_, sched);

    REQUIRE(wait_until(
        [&]
        {
            auto st = sched->get_status(id).state;
            return st == httplib::client::downloader::state::downloading;
        }));

    sched->pause(id);
    REQUIRE(wait_until(
        [&]
        {
            auto st = sched->get_status(id).state;
            return st == httplib::client::downloader::state::paused;
        }));

    // Cancel must wake the paused coroutine; shutdown drains to terminal.
    shutdown_scheduler(ts.ioc_, sched);

    REQUIRE(sched->active_count() == 0);
    REQUIRE(sched->pending_count() == 0);
    REQUIRE(is_terminal(sched->get_status(id).state));

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Download scheduler: shutdown while a task is pending", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_spend_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            resp.set_file_content(server_path);
        });
    ts.router().set_http_handler<http::verb::head>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(
        ts.ioc_.get_executor(), ts.pool, httplib::client::download_scheduler::scheduler_config { .max_concurrent = 1 });

    auto out1 = fs::temp_directory_path() / "sched_spend_out1.bin";
    auto out2 = fs::temp_directory_path() / "sched_spend_out2.bin";
    auto id1 = sched->add(ts.url_for_path("/slow"), out1);
    auto id2 = sched->add(ts.url_for_path("/slow"), out2);
    start_scheduler(ts.ioc_, sched);

    REQUIRE(wait_until([&] { return sched->pending_count() > 0; }));

    shutdown_scheduler(ts.ioc_, sched);

    // Pending tasks are cancelled and never started; the running one is also
    // cancelled to guarantee a quiescent exit.
    REQUIRE(sched->active_count() == 0);
    REQUIRE(sched->pending_count() == 0);
    REQUIRE(is_terminal(sched->get_status(id1).state));
    REQUIRE(sched->get_status(id2).state == httplib::client::downloader::state::cancelled);

    fs::remove(server_path);
    fs::remove(out1);
    std::error_code ec;
    fs::remove(out2, ec);
}

TEST_CASE("Download scheduler: add is rejected after shutdown", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_after_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "after shutdown\n";
    }
    auto dl_path = fs::temp_directory_path() / "sched_after_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/a",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/a",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "15");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);
    start_scheduler(ts.ioc_, sched);

    shutdown_scheduler(ts.ioc_, sched);

    // A shut-down scheduler must not register new tasks: the returned id is
    // never inserted, so the snapshot stays empty.
    auto id = sched->add(ts.url_for_path("/a"), dl_path);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto st = sched->get_status(id);
    REQUIRE(st.id == 0);
    REQUIRE(sched->total_count() == 0);

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Download scheduler: destructor without async_shutdown is safe", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_dtor_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "dtor\n";
    }
    auto dl_path = fs::temp_directory_path() / "sched_dtor_out.bin";

    net::io_context ioc;
    std::thread worker([&] { ioc.run(); });

    auto pool = std::make_shared<httplib::client::http_client_pool>(ioc.get_executor(),
                                                                    httplib::client::pool_params { .max_size = 4 });
    pool->start();

    {
        auto sched = std::make_shared<httplib::client::download_scheduler>(ioc.get_executor(), pool);
        auto id = sched->add("http://127.0.0.1:1/unreachable", dl_path);
        REQUIRE(id > 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // scope exit: the destructor runs request_stop() and cancels pending.
    }

    // Give the recently posted request_stop a chance to settle.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    pool->stop();
    ioc.stop();
    worker.join();

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Download scheduler: dynamic config change wakes dispatch", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_cfg_srv.bin";
    constexpr std::size_t kSize = 1024 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    std::atomic<int> active { 0 };
    std::atomic<int> max_seen { 0 };

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            int cur = active.fetch_add(1) + 1;
            int prev = max_seen.load();
            while (cur > prev && !max_seen.compare_exchange_weak(prev, cur))
            {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            resp.set_file_content(server_path);
            active.fetch_sub(1);
        });
    ts.router().set_http_handler<http::verb::head>(
        "/slow",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(
        ts.ioc_.get_executor(), ts.pool, httplib::client::download_scheduler::scheduler_config { .max_concurrent = 1 });

    auto out1 = fs::temp_directory_path() / "sched_cfg_out1.bin";
    auto out2 = fs::temp_directory_path() / "sched_cfg_out2.bin";
    auto id1 = sched->add(ts.url_for_path("/slow"), out1);
    auto id2 = sched->add(ts.url_for_path("/slow"), out2);
    start_scheduler(ts.ioc_, sched);

    // Only one runs at a time with max_concurrent=1; the second stays pending.
    REQUIRE(wait_until([&] { return sched->active_count() == 1; }));
    REQUIRE(sched->pending_count() == 1);

    // Raising max_concurrent should immediately dispatch the pending task.
    sched->set_scheduler_config(httplib::client::download_scheduler::scheduler_config { .max_concurrent = 2 });
    REQUIRE(sched->get_scheduler_config().max_concurrent == 2);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id1).state); }, 15000));
    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id2).state); }, 15000));
    REQUIRE(sched->get_status(id1).state == httplib::client::downloader::state::completed);
    REQUIRE(sched->get_status(id2).state == httplib::client::downloader::state::completed);
    REQUIRE(max_seen.load() <= 2);

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(out1);
    fs::remove(out2);
}

TEST_CASE("Download scheduler: state callback may re-enter add", "[download_scheduler]")
{
    auto srv_a = fs::temp_directory_path() / "sched_rentry_a.txt";
    auto srv_b = fs::temp_directory_path() / "sched_rentry_b.txt";
    {
        std::ofstream fa(srv_a, std::ios::binary);
        fa << "first\n";
        std::ofstream fb(srv_b, std::ios::binary);
        fb << "second\n";
    }
    auto out_a = fs::temp_directory_path() / "sched_rentry_out_a.bin";
    auto out_b = fs::temp_directory_path() / "sched_rentry_out_b.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/a",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(srv_a); });
    ts.router().set_http_handler<http::verb::head>(
        "/a",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "6");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.router().set_http_handler<http::verb::get>(
        "/b",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(srv_b); });
    ts.router().set_http_handler<http::verb::head>(
        "/b",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "7");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);

    std::atomic<httplib::client::download_scheduler::task_id> second_id { 0 };
    std::atomic<bool> first_completed { false };
    sched->set_state_callback(
        [&](httplib::client::download_scheduler::task_status const& s)
        {
            if (s.state == httplib::client::downloader::state::completed && s.save_path == out_a
                && !first_completed.exchange(true))
            {
                // Re-enter add() from inside the strand callback.
                second_id.store(sched->add(ts.url_for_path("/b"), out_b));
            }
        });

    auto id_a = sched->add(ts.url_for_path("/a"), out_a);
    start_scheduler(ts.ioc_, sched);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id_a).state); }));
    REQUIRE(second_id.load() > 0);

    std::uint64_t sid = second_id.load();
    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(sid).state); }, 15000));
    REQUIRE(sched->get_status(sid).state == httplib::client::downloader::state::completed);
    REQUIRE(read_file(out_b) == "second\n");

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(srv_a);
    fs::remove(srv_b);
    fs::remove(out_a);
    fs::remove(out_b);
}

TEST_CASE("Download scheduler: callback exception does not kill the run", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_chex_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "exc\n";
    }
    auto dl_path = fs::temp_directory_path() / "sched_chex_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/e",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/e",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "4");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);

    sched->set_state_callback(
        [](httplib::client::download_scheduler::task_status const&)
        {
            throw std::runtime_error("boom");
        });

    auto id = sched->add(ts.url_for_path("/e"), dl_path);
    start_scheduler(ts.ioc_, sched);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id).state); }));
    REQUIRE(sched->get_status(id).state == httplib::client::downloader::state::completed);
    REQUIRE(read_file(dl_path) == "exc\n");

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Download scheduler: callback may trigger shutdown", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_ctor_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "ctor\n";
    }
    auto dl_path = fs::temp_directory_path() / "sched_ctor_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/g",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/g",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "5");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);

    std::atomic<bool> shutdown_started { false };
    sched->set_state_callback(
        [&](httplib::client::download_scheduler::task_status const& s)
        {
            if (s.state == httplib::client::downloader::state::completed && !shutdown_started.exchange(true))
            {
                net::co_spawn(
                    ts.ioc_,
                    [sched]() -> net::awaitable<void> { co_await sched->async_shutdown(); },
                    net::detached);
            }
        });

    sched->add(ts.url_for_path("/g"), dl_path);

    std::promise<void> run_done;
    auto run_future = run_done.get_future();
    net::co_spawn(
        ts.ioc_,
        [sched, &run_done]() -> net::awaitable<void>
        {
            co_await sched->async_run();
            run_done.set_value();
        },
        net::detached);

    // async_run must return once the callback-triggered shutdown drains.
    REQUIRE(run_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(read_file(dl_path) == "ctor\n");

    fs::remove(server_path);
    fs::remove(dl_path);
}

TEST_CASE("Download scheduler: concurrent API calls from many threads", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_mt_srv.bin";
    constexpr std::size_t kSize = 512 * 1024;
    {
        std::ofstream f(server_path, std::ios::binary);
        for (std::size_t i = 0; i < kSize; ++i)
        {
            f.put(static_cast<char>(i % 256));
        }
    }

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/m",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/m",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(
        ts.ioc_.get_executor(), ts.pool, httplib::client::download_scheduler::scheduler_config { .max_concurrent = 4 });

    // Seed a task first so async_run never observes an empty task set and
    // exits before the worker threads start submitting.
    auto seed_out = fs::temp_directory_path() / "sched_mt_seed.bin";
    sched->add(ts.url_for_path("/m"), seed_out);
    start_scheduler(ts.ioc_, sched);

    constexpr int kThreads = 4;
    constexpr int kIterations = 20;
    std::vector<std::thread> threads;
    std::vector<std::vector<httplib::client::download_scheduler::task_id>> added(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [&, t]()
            {
                for (int i = 0; i < kIterations; ++i)
                {
                    auto out = fs::temp_directory_path() / std::format("sched_mt_{}_{}.bin", t, i);
                    auto id = sched->add(ts.url_for_path("/m"), out);
                    added[t].push_back(id);
                    if (i % 3 == 0)
                    {
                        sched->pause(id);
                        sched->resume(id);
                    }
                    if (i % 5 == 0)
                    {
                        sched->cancel(id);
                    }
                    (void)sched->get_scheduler_config().max_concurrent;
                    (void)sched->active_count();
                    (void)sched->pending_count();
                    (void)sched->total_count();
                }
            });
    }

    for (auto& th : threads)
    {
        th.join();
    }

    // A random grab-bag of tweaks while tasks are still in flight.
    sched->set_scheduler_config(httplib::client::download_scheduler::scheduler_config { .max_concurrent = 8 });
    sched->set_scheduler_config(httplib::client::download_scheduler::scheduler_config { .max_concurrent = 2 });
    (void)sched->get_all_status();

    // Drain everything to a quiescent state.
    REQUIRE(wait_until(
        [&]
        {
            for (auto const& s : sched->get_all_status())
            {
                if (!is_terminal(s.state))
                {
                    return false;
                }
            }
            return true;
        },
        30000));

    shutdown_scheduler(ts.ioc_, sched);
    REQUIRE(sched->active_count() == 0);
    REQUIRE(sched->pending_count() == 0);

    fs::remove(server_path);
    std::error_code ec;
    fs::remove(seed_out, ec);
    for (int t = 0; t < kThreads; ++t)
    {
        for (int i = 0; i < kIterations; ++i)
        {
            std::error_code ec;
            fs::remove(fs::temp_directory_path() / std::format("sched_mt_{}_{}.bin", t, i), ec);
        }
    }
}

TEST_CASE("Download scheduler: sync queries are safe from inside a state callback", "[download_scheduler]")
{
    auto server_path = fs::temp_directory_path() / "sched_rq_srv.txt";
    {
        std::ofstream f(server_path, std::ios::binary);
        f << "reentrant queries\n";
    }
    auto dl_path = fs::temp_directory_path() / "sched_rq_out.bin";

    dl_sched_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/q",
        [&](httplib::server::request&, httplib::server::response& resp)
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/q",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, "18");
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);
    auto id = sched->add(ts.url_for_path("/q"), dl_path);

    std::atomic<bool> queried_ok { false };
    sched->set_state_callback(
        [&](httplib::client::download_scheduler::task_status const& s)
        {
            if (s.state != httplib::client::downloader::state::completed)
            {
                return;
            }
            auto st = sched->get_status(id);
            auto all = sched->get_all_status();
            (void)sched->active_count();
            (void)sched->pending_count();
            auto total = sched->total_count();
            auto cfg = sched->get_scheduler_config();
            queried_ok.store(
                st.id == id && all.size() == 1 && total == 1 && cfg.max_concurrent >= 1,
                std::memory_order_relaxed);
        });
    start_scheduler(ts.ioc_, sched);

    REQUIRE(wait_until([&] { return is_terminal(sched->get_status(id).state); }));
    REQUIRE(queried_ok.load());
    REQUIRE(sched->get_status(id).state == httplib::client::downloader::state::completed);
    REQUIRE(read_file(dl_path) == "reentrant queries\n");

    shutdown_scheduler(ts.ioc_, sched);

    fs::remove(server_path);
    fs::remove(dl_path);
}