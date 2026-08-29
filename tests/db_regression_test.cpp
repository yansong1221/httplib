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
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/errc.hpp>
#include <catch2/catch_test_macros.hpp>
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
        net::co_spawn(
            ex,
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
        int fail_connect_at = -1; ///< 第几次 connect 抛异常；-1 = 从不
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
            co_return detail::statement_handle { std::make_shared<int>(0) };
        }
        net::awaitable<db::result>
        execute_statement(detail::statement_handle, std::vector<db::param> const&) override
        {
            co_return db::result {};
        }
        net::awaitable<void>
        close_statement(detail::statement_handle h) noexcept override
        {
            h.state.reset();
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
    db::result one({
        db::result::resultset { { { db::field { static_cast<int64_t>(9) } } }, { "v" }, { db::column_type::int64 } }
    });
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

            db::connection_pool pool = db::make_pool(ex, backend_name, "", cfg);
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
            net::co_spawn(
                ex,
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
            net::co_spawn(
                ex,
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

            bool done
                = co_await co_wait_for(ex, std::chrono::seconds(3), [&] { return r3->completed && r4->completed; });
            REQUIRE(done);

            // 修复前：c3 失败时没有唤醒其他等待者，c4 一直睡到 800ms 超时（acquire 抛）。
            REQUIRE_FALSE(r3->success);
            REQUIRE(r4->success);

            pool.stop();
        });
}

// ---- bug 2（高）：健康检查期间待 ping 连接不占槽位，池可短暂超过 max_connections，total_count 少报 ----
// 修复：验证中的连接计入容量（validating_）。验证窗口内池满时借出方必须等待（或超时），
// 不得超建连接；total_count 全程如实反映物理连接数。

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

            db::connection_pool pool = db::make_pool(ex, backend_name, "", cfg);
            pool.start();

            // 1) 预建 min 条连接。
            bool pre = co_await co_wait_for(ex, std::chrono::seconds(3), [&] { return pool.total_count() == 2; });
            REQUIRE(pre);
            REQUIRE(pool.idle_count() == 2);

            // 2) 阻塞 ping → 维护协程把空闲连接全部拉去健康检查。
            //    修复后：被验证的连接经 validating_ 占容量，total 保持 2（修复前少报为 0）。
            ctrl->block_ping = true;
            bool pulled = co_await co_wait_for(ex, std::chrono::seconds(3), [&] { return pool.idle_count() == 0; });
            REQUIRE(pulled);
            REQUIRE(pool.total_count() == 2);

            // 3) 借出：池已满（2 条在验证）→ 必须等待；修复前此处会超建第 3 条。
            std::optional<db::connection_pool::session_handle> bg;
            std::atomic_bool bg_done = false;
            net::co_spawn(
                ex,
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
            REQUIRE_FALSE(bg.has_value());    // 等待 500ms 超时，未超建
            REQUIRE(pool.total_count() == 2); // 物理连接仍只有 2 条

            // 4) 放开 ping → 健康检查完成、连接回填，随后借出正常。
            ctrl->block_ping = false;
            bool back = co_await co_wait_for(ex, std::chrono::seconds(3), [&] { return pool.idle_count() == 2; });
            REQUIRE(back);
            auto bg2 = co_await pool.async_acquire(std::chrono::seconds(3));
            CHECK(pool.total_count() == 2);
            bg2.release();
            REQUIRE(pool.total_count() <= cfg.max_connections);

            pool.stop();
        });
}

// ---- 增强：健康检查剔除死连接释放槽位时必须唤醒等待者（修复前等待者睡到超时）----

TEST_CASE("db: pool wakes waiter when health check discards a connection", "[db][regression]")
{
    auto ctrl = std::make_shared<fake_ctrl>();
    constexpr char const* backend_name = "fake_discard_wake";
    REQUIRE(register_fake(backend_name, ctrl));

    run_on_io_context(
        [ctrl, backend_name](net::any_io_executor ex) -> net::awaitable<void>
        {
            db::pool_params cfg;
            cfg.min_connections = 0;
            cfg.max_connections = 1;
            cfg.idle_check_interval = std::chrono::milliseconds(50);
            cfg.health_check_interval = std::chrono::seconds(300); // 首 tick 靠 since_ping=∞ 触发，之后测试期内不再拉
            cfg.idle_timeout = std::chrono::seconds(0);

            db::connection_pool pool = db::make_pool(ex, backend_name, "", cfg);
            pool.start();

            // 1) 借出再归还，得到一条空闲连接。
            {
                auto h = co_await pool.async_acquire();
            }
            REQUIRE(pool.total_count() == 1);
            REQUIRE(pool.idle_count() == 1);

            // 2) 阻塞 ping → 维护协程把该连接拉去健康检查（占容量 → 池视为满）。
            ctrl->block_ping = true;
            bool pulled = co_await co_wait_for(ex, std::chrono::seconds(3), [&] { return pool.idle_count() == 0; });
            REQUIRE(pulled);
            REQUIRE(pool.total_count() == 1);

            // 3) 此时借出必须等待，不得超建（修复前会误判池空而直接新建）。
            struct waiter_result
            {
                bool completed = false;
                bool success = false;
                std::chrono::milliseconds elapsed { 0 };
            };
            auto r = std::make_shared<waiter_result>();
            auto start = std::chrono::steady_clock::now();
            net::co_spawn(
                ex,
                [r, start, &pool]() -> net::awaitable<void>
                {
                    try
                    {
                        auto h = co_await pool.async_acquire(std::chrono::milliseconds(2000));
                        (void)h;
                        r->success = true;
                    }
                    catch (...)
                    {
                    }
                    r->elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()
                                                                                       - start);
                    r->completed = true;
                },
                [](std::exception_ptr) {});

            // 让等待者进入队列；池满状态下它应仍在等待。
            co_await co_sleep(ex, std::chrono::milliseconds(100));
            REQUIRE_FALSE(r->completed);

            // 4) 放开 ping 且判死 → 连接被剔除、槽位释放，应立刻唤醒等待者。
            ctrl->ping_ok = false;
            ctrl->block_ping = false;

            bool done = co_await co_wait_for(ex, std::chrono::seconds(3), [&] { return r->completed; });
            REQUIRE(done);
            REQUIRE(r->success);                                   // 被唤醒后新建连接借出成功
            REQUIRE(r->elapsed < std::chrono::milliseconds(1500)); // 靠剔除唤醒，而非睡满 2s 超时
            REQUIRE(pool.total_count() == 1);                      // 池中只剩新连接

            pool.stop();
        });
}

// ---- 增强：acquire 失败按 db_exception::code 分类（timed_out / operation_canceled）----

TEST_CASE("db: pool acquire errors are typed db_exceptions", "[db][regression]")
{
    auto ctrl = std::make_shared<fake_ctrl>();
    constexpr char const* backend_name = "fake_pool_errors";
    REQUIRE(register_fake(backend_name, ctrl));

    run_on_io_context(
        [ctrl, backend_name](net::any_io_executor ex) -> net::awaitable<void>
        {
            db::pool_params cfg;
            cfg.min_connections = 0;
            cfg.max_connections = 1;
            cfg.idle_check_interval = std::chrono::seconds(10);
            cfg.health_check_interval = std::chrono::seconds(0);
            cfg.idle_timeout = std::chrono::seconds(0);

            db::connection_pool pool = db::make_pool(ex, backend_name, "", cfg);

            // 未启动 → operation_canceled。
            try
            {
                auto h = co_await pool.async_acquire(std::chrono::milliseconds(50));
                (void)h;
                FAIL("expected pool_closed");
            }
            catch (db::db_exception const& e)
            {
                CHECK(e.code() == boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
            }

            pool.start();

            // 池满 + 等待超时 → timed_out。
            {
                auto h1 = co_await pool.async_acquire();
                try
                {
                    auto h = co_await pool.async_acquire(std::chrono::milliseconds(100));
                    (void)h;
                    FAIL("expected timeout");
                }
                catch (db::db_exception const& e)
                {
                    CHECK(e.code() == boost::system::errc::make_error_code(boost::system::errc::timed_out));
                }
                h1.release();
            }

            // fail fast（wait_timeout=0）→ 同样 timed_out。
            {
                auto h2 = co_await pool.async_acquire();
                try
                {
                    auto h = co_await pool.async_acquire(std::chrono::steady_clock::duration::zero());
                    (void)h;
                    FAIL("expected fail-fast timeout");
                }
                catch (db::db_exception const& e)
                {
                    CHECK(e.code() == boost::system::errc::make_error_code(boost::system::errc::timed_out));
                }
                h2.release();
            }

            // 已停止 → operation_canceled。
            pool.stop();
            try
            {
                auto h = co_await pool.async_acquire(std::chrono::milliseconds(50));
                (void)h;
                FAIL("expected pool_closed");
            }
            catch (db::db_exception const& e)
            {
                CHECK(e.code() == boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
            }
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

// ---- 异常统一：渲染/访问层错误统一抛 db_exception（此前部分为 std::runtime_error，绕过 session 错误管道） ----

TEST_CASE("db: render errors are typed db_exception", "[db][regression]")
{
    stub_backend def;

    // SQL 有 :a 占位符但未提供任何绑定 → 未绑定命名参数
    REQUIRE_THROWS_AS(detail::render_query("SELECT :a AS x", {}, def), db::db_exception);

    // 位置绑定与命名绑定混用 → 禁止
    REQUIRE_THROWS_AS(detail::render_query("SELECT :a AS x", { db::bind(1), db::bind("a", 2) }, def), db::db_exception);
}

TEST_CASE("db: row/result access errors are typed db_exception, bounds keep std::out_of_range", "[db][regression]")
{
    db::result r({
        db::result::resultset { { { db::field { std::string("hello") } } }, { "v" }, { db::column_type::string } }
    });

    // 类型不匹配 → db_exception（此前为 std::runtime_error）
    REQUIRE_THROWS_AS(r[0].as_int64(0), db::db_exception);
    REQUIRE_THROWS_AS(r[0].as_blob(0), db::db_exception);

    // field{std::string} 走 text 拥有型构造（非借用无锚点），值可读且不悬垂
    REQUIRE(r[0].as_string(0) == "hello");

    // 列名不存在 → db_exception（此前为 std::runtime_error）
    REQUIRE_THROWS_AS(r.column_index("nope"), db::db_exception);

    // 下标越界 → 保留标准容器语义 std::out_of_range
    REQUIRE_THROWS_AS(r[1].as_int64(0), std::out_of_range);
    REQUIRE_THROWS_AS(r[0].as_int64(999), std::out_of_range);
}

// ---- 多语句：参数化语句统一在 session 层拒绝（此前仅 SQLite 后端检测，ODBC 有静默截断风险） ----

TEST_CASE("db: split_statements splits on statement-level semicolons", "[db][regression]")
{
    using detail::split_statements;

    // 单语句（无分号）
    {
        auto v = split_statements("SELECT 1");
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == "SELECT 1");
    }
    // 末尾分号不产生空语句
    {
        auto v = split_statements("SELECT 1;");
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == "SELECT 1");
    }
    // 多语句按顺序拆分
    {
        auto v = split_statements("SELECT 1; SELECT 2; SELECT 3");
        REQUIRE(v.size() == 3);
        REQUIRE(v[0] == "SELECT 1");
        REQUIRE(v[1] == "SELECT 2");
        REQUIRE(v[2] == "SELECT 3");
    }
    // 字符串内的分号不拆
    {
        auto v = split_statements("SELECT 'a;b' AS x");
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == "SELECT 'a;b' AS x");
    }
    // 双引号引用内的分号不拆
    {
        auto v = split_statements("SELECT \"a;b\" AS x");
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == "SELECT \"a;b\" AS x");
    }
    // 反引号引用（MySQL 标识符）内的分号不拆
    {
        auto v = split_statements("SELECT `a;b` FROM t");
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == "SELECT `a;b` FROM t");
    }
    // 引号内转义的双引号（'' 表示单引号字面量）
    {
        auto v = split_statements("SELECT 'it''s;fine'");
        REQUIRE(v.size() == 1);
    }
    // 行注释内的分号不拆
    {
        auto v = split_statements("SELECT 1 -- ; still comment\n; SELECT 2");
        REQUIRE(v.size() == 2);
        REQUIRE(v[0] == "SELECT 1 -- ; still comment");
        REQUIRE(v[1] == "SELECT 2");
    }
    // 块注释内的分号不拆
    {
        auto v = split_statements("SELECT 1 /* ; ; */ ; SELECT 2");
        REQUIRE(v.size() == 2);
    }
    // 空语句（连续/首尾分号）跳过
    {
        auto v = split_statements(";; SELECT 1 ;;");
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == "SELECT 1");
    }
}

TEST_CASE("db: render keeps ''-escaped strings intact", "[db][regression]")
{
    stub_backend def;
    // 字符串内（'' 转义后）的 :name 是字面量，不应被当作占位符
    auto r = detail::render_query("SELECT 'it''s:name' AS x", {}, def);
    REQUIRE(r.sql == "SELECT 'it''s:name' AS x");
    REQUIRE(r.params.empty());
}

TEST_CASE("db: parameterized multi-statement is rejected at session layer", "[db][regression]")
{
    auto ctrl = std::make_shared<fake_ctrl>();
    constexpr char const* backend_name = "fake_multi_stmt";
    REQUIRE(register_fake(backend_name, ctrl));

    run_on_io_context(
        [ctrl, backend_name](net::any_io_executor ex) -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ex, backend_name, "");

            // fake 后端不解析 SQL，纯靠 session 层统一检测拒绝参数化多语句
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x; SELECT :b AS y").bind("a", 1).bind("b", 2).execute(),
                              db::db_exception);

            // 单语句参数化正常（prepare 只调一次）
            ctrl->prepare_calls.store(0);
            co_await sess.stmt("SELECT :a AS x").bind("a", 1).execute();
            REQUIRE(ctrl->prepare_calls.load() == 1);
        });
}
#endif
