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
#include <filesystem>
#include <format>
#include <fstream>
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
    start_scheduler(ts.ioc_, sched);

    auto id = sched->add(ts.url_for_path("/file"), dl_path);
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

    start_scheduler(ts.ioc_, sched);

    std::vector<fs::path> outs;
    for (int i = 0; i < 3; ++i)
    {
        outs.push_back(fs::temp_directory_path() / std::format("sched_conc_out_{}.bin", i));
        sched->add(ts.url_for_path("/slow"), outs.back());
    }

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
        { resp.set_file_content(server_path); });
    ts.router().set_http_handler<http::verb::head>(
        "/big",
        [&](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(http::field::content_length, std::to_string(kSize));
            resp.set(http::field::accept_ranges, "bytes");
        });
    ts.start();

    auto sched = std::make_shared<httplib::client::download_scheduler>(ts.ioc_.get_executor(), ts.pool);
    start_scheduler(ts.ioc_, sched);

    std::vector<fs::path> outs;
    for (int i = 0; i < 3; ++i)
    {
        outs.push_back(fs::temp_directory_path() / std::format("sched_cancel_out_{}.bin", i));
        sched->add(ts.url_for_path("/big"), outs.back());
    }

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
    start_scheduler(ts.ioc_, sched);

    auto dl_path = fs::temp_directory_path() / "sched_pause_out.bin";
    auto id = sched->add(ts.url_for_path("/p"), dl_path);

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

    start_scheduler(ts.ioc_, sched);

    auto id = sched->add(ts.url_for_path("/cb"), dl_path);

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
    start_scheduler(ts.ioc_, sched);

    auto id_a = sched->add(ts.url_for_path("/a"), out_a);

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
        { resp.set_file_content(server_path); });
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

    start_scheduler(ts.ioc_, sched);

    auto out1 = fs::temp_directory_path() / "sched_pend_out1.bin";
    auto out2 = fs::temp_directory_path() / "sched_pend_out2.bin";

    auto id1 = sched->add(ts.url_for_path("/slow"), out1);
    auto id2 = sched->add(ts.url_for_path("/slow"), out2);

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
    start_scheduler(ts.ioc_, sched);

    sched->add(ts.url_for_path("/done"), dl_path);

    shutdown_scheduler(ts.ioc_, sched);

    // After shutdown, the scheduler should be quiescent.
    REQUIRE(sched->active_count() == 0);
    REQUIRE(sched->pending_count() == 0);

    fs::remove(server_path);
    fs::remove(dl_path);
}