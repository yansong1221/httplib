#ifdef HTTPLIB_ENABLED_DATABASE
#include "db/backend.hpp"
#include "db/registry.hpp"
#include "db/render.hpp"
#include "httplib/db/binder.hpp"
#include "httplib/db/connection_pool.hpp"
#include "httplib/db/exception.hpp"
#include "httplib/db/extractor.hpp"
#include "httplib/db/options.hpp"
#include "httplib/db/result.hpp"
#include "httplib/db/session.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace db = httplib::db;
namespace net = httplib::net;

namespace
{
    namespace detail = db::detail;

    // ---- 工具 ----

    /// 在单线程 io_context 上运行协程；协程抛出的异常重新抛到测试线程。
    template <typename Fn>
    void
    run_on_io_context(Fn&& fn)
    {
        net::io_context ioc;
        auto ex = ioc.get_executor();
        std::exception_ptr err;
        net::co_spawn(ex,
                      [fn = std::forward<Fn>(fn), ex]() mutable -> net::awaitable<void> { co_await fn(ex); },
                      [&](std::exception_ptr e) { err = e; });
        ioc.run();
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

    net::awaitable<void>
    co_sleep(net::any_io_executor ex, std::chrono::milliseconds ms)
    {
        net::steady_timer t(std::move(ex));
        t.expires_after(ms);
        co_await t.async_wait(net::use_awaitable);
    }

    net::awaitable<bool>
    co_wait_for(net::any_io_executor ex, std::chrono::milliseconds ms, std::function<bool()> pred)
    {
        auto const deadline = std::chrono::steady_clock::now() + ms;
        while (!pred())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                co_return false;
            }
            co_await co_sleep(ex, std::chrono::milliseconds(20));
        }
        co_return true;
    }

    // ---- 纯渲染测试用最小后端（同 sqlite_test.cpp 的 stub） ----

    struct stub_backend : detail::backend
    {
        net::awaitable<void>
        connect() override
        {
            co_return;
        }
        net::awaitable<bool>
        ping() override
        {
            co_return true;
        }
        net::awaitable<db::result>
        execute(std::string_view) override
        {
            co_return db::result {};
        }
        net::awaitable<detail::statement_handle>
        prepare(std::string_view) override
        {
            co_return detail::statement_handle {};
        }
        net::awaitable<db::result>
        execute_statement(detail::statement_handle, std::vector<db::param> const&) override
        {
            co_return db::result {};
        }
        net::awaitable<void>
        close_statement(detail::statement_handle) noexcept override
        {
            co_return;
        }
        net::awaitable<void>
        begin() override
        {
            co_return;
        }
        net::awaitable<void>
        commit() override
        {
            co_return;
        }
        net::awaitable<void>
        rollback() override
        {
            co_return;
        }
    };

    // ---- 连接池 / 会话测试用假后端 ----

    struct fake_ctrl
    {
        std::atomic_bool block_ping { false }; ///< ping 挂起（模拟慢健康检查）
        std::atomic_bool ping_ok { true };     ///< ping 返回值（false → 连接判死）
        std::atomic<int> connect_calls { 0 };
        std::atomic<int> prepare_calls { 0 };
        int fail_connect_at = -1;              ///< 第几次 connect 抛异常；-1 = 从不
    };

    struct fake_backend : detail::backend
    {
        net::any_io_executor ex;
        std::shared_ptr<fake_ctrl> ctrl;

        fake_backend(net::any_io_executor e, std::shared_ptr<fake_ctrl> c) : ex(std::move(e)), ctrl(std::move(c)) {}

        net::awaitable<void>
        connect() override
        {
            if (ctrl->fail_connect_at > 0 && ++ctrl->connect_calls == ctrl->fail_connect_at)
            {
                throw db::db_exception(boost::system::error_code {}, "fake: connect failed");
            }
            co_return;
        }
        net::awaitable<bool>
        ping() override
        {
            while (ctrl->block_ping.load())
            {
                co_await co_sleep(ex, std::chrono::milliseconds(10));
            }
            co_return ctrl->ping_ok.load();
        }
        net::awaitable<db::result>
        execute(std::string_view) override
        {
            co_return db::result {};
        }
        net::awaitable<detail::statement_handle>
        prepare(std::string_view) override
        {
            ++ctrl->prepare_calls;
            co_return detail::statement_handle { new int };
        }
        net::awaitable<db::result>
        execute_statement(detail::statement_handle, std::vector<db::param> const&) override
        {
            co_return db::result {};
        }
        net::awaitable<void>
        close_statement(detail::statement_handle h) noexcept override
        {
            delete static_cast<int*>(h.state);
            co_return;
        }
        net::awaitable<void>
        begin() override
        {
            co_return;
        }
        net::awaitable<void>
        commit() override
        {
            co_return;
        }
        net::awaitable<void>
        rollback() override
        {
            co_return;
        }
    };

    bool
    register_fake(char const* name, std::shared_ptr<fake_ctrl> ctrl)
    {
        return detail::register_backend(
            name,
            [ctrl](net::any_io_executor ex, db::options const&) -> std::unique_ptr<detail::backend>
            { return std::make_unique<fake_backend>(std::move(ex), ctrl); });
    }
} // namespace

// ---- bug 4（中）：into(vector) 在空结果时不清空已有数据 ----

TEST_CASE("db: into(vector) is cleared on empty result", "[db][regression]")
{
    // 修复前：row_count()==0 时 apply_extractors 提前 return，不清空已有 vector。
    std::vector<int64_t> ids { 1, 2, 3 };
    db::result empty(std::vector<db::result::resultset> {});
    db::detail::apply_extractors(empty, db::into(ids));
    REQUIRE(ids.empty());

    // sanity：非空结果先 clear 再填充。
    db::result one({ db::result::resultset { { { db::field { static_cast<int64_t>(9) } } },
                                              { "v" },
                                              { db::column_type::int64 } } });
    std::vector<int64_t> vals { 1 };
    db::detail::apply_extractors(one, db::into(vals));
    REQUIRE(vals == std::vector<int64_t> { 9 });
}

// ---- bug 6（低）：to_connection_string 未对含换行的值加引号，round-trip 截断 ----

TEST_CASE("db: to_connection_string quotes newline values", "[db][regression]")
{
    db::options o;
    o.set("note", "line1\nline2");
    auto s = o.to_connection_string();

    // 修复前：换行不在引用判定集合里，序列化不带引号，解析时在换行处截断。
    REQUIRE(s.find('"') != std::string::npos);
    auto parsed = db::options::parse(s);
    REQUIRE(parsed.get_or("note") == "line1\nline2");
}

// ---- bug 3（高）：连接创建失败时未唤醒其他等待者，后者睡到超时 ----

TEST_CASE("db: pool wakes waiters when connection creation fails", "[db][regression]")
{
    auto ctrl = std::make_shared<fake_ctrl>();
    ctrl->fail_connect_at = 3; // 第 1、2 次成功（h1/h2），第 3 次失败（被唤醒的 c3）

    constexpr char const* backend_name = "fake_wake_waiter";
    REQUIRE(register_fake(backend_name, ctrl));

    run_on_io_context(
        [ctrl, backend_name](net::any_io_executor ex) -> net::awaitable<void>
        {
            db::pool_params cfg;
            cfg.min_connections = 0;
            cfg.max_connections = 2;
            cfg.idle_check_interval = std::chrono::seconds(10);  // 维护协程不干扰本用例
            cfg.health_check_interval = std::chrono::seconds(0); // 禁用健康检查
            cfg.idle_timeout = std::chrono::seconds(0);

            db::connection_pool pool = db::make_pool(ex, cfg, backend_name, "");
            pool.start();

            auto h1 = co_await pool.async_acquire();
            auto h2 = co_await pool.async_acquire(); // 池满：active=2

            struct waiter_result
            {
                bool completed = false;
                bool success = false;
            };
            auto r3 = std::make_shared<waiter_result>();
            auto r4 = std::make_shared<waiter_result>();
            net::co_spawn(ex,
                          [&pool, r3]() -> net::awaitable<void>
                          {
                              try
                              {
                                  auto h = co_await pool.async_acquire(std::chrono::milliseconds(800));
                                  (void)h;
                                  r3->success = true;
                              }
                              catch (...)
                              {
                              }
                              r3->completed = true;
                          },
                          [](std::exception_ptr) {});
            net::co_spawn(ex,
                          [&pool, r4]() -> net::awaitable<void>
                          {
                              try
                              {
                                  auto h = co_await pool.async_acquire(std::chrono::milliseconds(800));
                                  (void)h;
                                  r4->success = true;
                              }
                              catch (...)
                              {
                              }
                              r4->completed = true;
                          },
                          [](std::exception_ptr) {});

            // 让两个等待者都注册进 waiters_。
            co_await co_sleep(ex, std::chrono::milliseconds(150));

            // 令 h1 连接失效并释放 → 唤醒一个等待者（c3）→ c3 新建连接失败。
            ctrl->ping_ok = false;
            co_await h1->ping(); // live = false
            h1.release();

            bool done = co_await co_wait_for(
                ex, std::chrono::seconds(3), [&] { return r3->completed && r4->completed; });
            REQUIRE(done);

            // 修复前：c3 失败时没有唤醒其他等待者，c4 一直睡到 800ms 超时（acquire 抛）。
            REQUIRE_FALSE(r3->success);
            REQUIRE(r4->success);

            pool.stop();
        });
}

// ---- bug 2（高）：健康检查期间待 ping 连接不占槽位，池可短暂超过 max_connections ----

TEST_CASE("db: pool stays within max_connections during health checks", "[db][regression]")
{
    auto ctrl = std::make_shared<fake_ctrl>();
    constexpr char const* backend_name = "fake_health_check";
    REQUIRE(register_fake(backend_name, ctrl));

    run_on_io_context(
        [ctrl, backend_name](net::any_io_executor ex) -> net::awaitable<void>
        {
            db::pool_params cfg;
            cfg.min_connections = 2;
            cfg.max_connections = 2;
            cfg.idle_check_interval = std::chrono::seconds(1);
            cfg.health_check_interval = std::chrono::seconds(10); // 首次拉取后长时间不重拉，保证断言稳定
            cfg.idle_timeout = std::chrono::seconds(0);

            db::connection_pool pool = db::make_pool(ex, cfg, backend_name, "");
            pool.start();

            // 1) 预建 min 条连接。
            bool pre = co_await co_wait_for(ex, std::chrono::seconds(3), [&] { return pool.total_count() == 2; });
            REQUIRE(pre);
            REQUIRE(pool.idle_count() == 2);

            // 2) 阻塞 ping → 维护协程把空闲连接全部拉去健康检查（此刻它们在池中不可见）。
            ctrl->block_ping = true;
            bool pulled = co_await co_wait_for(ex, std::chrono::seconds(3), [&] { return pool.idle_count() == 0; });
            REQUIRE(pulled);
            CHECK(pool.total_count() == 0); // 根因：待 ping 连接既不占 active 槽位也不在 idle_

            // 3) 借出：池内没有空闲连接 → 走 capacity 分支新建第 3 条（修复前突破 max_connections）。
            std::optional<db::connection_pool::session_handle> bg;
            std::atomic_bool bg_done = false;
            net::co_spawn(ex,
                          [&]() -> net::awaitable<void>
                          {
                              try
                              {
                                  bg.emplace(co_await pool.async_acquire(std::chrono::milliseconds(500)));
                              }
                              catch (...)
                              {
                              }
                              bg_done.store(true);
                          },
                          [](std::exception_ptr) {});
            bool acq = co_await co_wait_for(ex, std::chrono::seconds(3), [&] { return bg_done.load(); });
            REQUIRE(acq);
            REQUIRE(bg.has_value());

            // 4) 放开 ping 让维护协程回填；bg 归池前先 ping 一次，避免随后被健康检查拉走导致总数瞬时隐藏。
            ctrl->block_ping = false;
            co_await co_sleep(ex, std::chrono::milliseconds(150));
            co_await bg->get()->ping();
            bg->release();
            co_await co_sleep(ex, std::chrono::milliseconds(400));

            // 修复前：3 条 > max_connections；修复后必须不超过上限。
            REQUIRE(pool.total_count() <= cfg.max_connections);

            pool.stop();
        });
}

// ---- bug 5（中）：max_cached_statements 传负值被 cast 成 SIZE_MAX，语句缓存无上限 ----

TEST_CASE("db: negative max_cached_statements disables statement cache", "[db][regression]")
{
    auto ctrl = std::make_shared<fake_ctrl>();
    constexpr char const* backend_name = "fake_neg_cache";
    REQUIRE(register_fake(backend_name, ctrl));

    run_on_io_context(
        [ctrl, backend_name](net::any_io_executor ex) -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ex, backend_name, "max_cached_statements=-1");
            co_await sess.stmt("SELECT :a AS x").bind(1).execute();
            co_await sess.stmt("SELECT :a AS x").bind(2).execute();

            // 修复前：-1 → SIZE_MAX，第二条语句复用缓存句柄，prepare 只调一次。
            REQUIRE(ctrl->prepare_calls.load() == 2);
        });
}
#endif
