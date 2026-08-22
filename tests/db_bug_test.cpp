#ifdef HTTPLIB_ENABLED_DATABASE
// db 模块缺陷复现测试：
// 每个用例按"正确行为"断言，当前实现下应当失败（FAIL），用于在修复前验证缺陷存在；
// 修复一个缺陷后对应用例应转绿。运行：httplib_tests.exe "[db][bug]"
#include "httplib/db/binder.hpp"
#include "httplib/db/config.hpp"
#include "httplib/db/exception.hpp"
#include "httplib/db/session.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace db = httplib::db;
namespace net = httplib::net;

namespace
{
    /// 协程异常重抛到测试线程；connect_tag 非空且命中时视为"本地无对应数据库服务"而 SKIP。
    void
    rethrow_or_skip(std::exception_ptr err, std::string_view connect_tag)
    {
        if (!err)
        {
            return;
        }
        try
        {
            std::rethrow_exception(err);
        }
        catch (db::db_exception const& ex)
        {
            if (!connect_tag.empty() && std::string(ex.what()).find(connect_tag) != std::string::npos)
            {
                SKIP("no local database server");
            }
            throw;
        }
    }
} // namespace

// ---------------------------------------------------------------------------
// SQLite
// ---------------------------------------------------------------------------

TEST_CASE("db(sqlite/bug): temporal column decls recognized case-insensitively and with modifiers",
          "[db][sqlite][bug]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            // 小写声明与带修饰符的声明（datetime(3)）都应按声明首词识别列语义
            co_await sess.query("CREATE TABLE t (d date, dt datetime, tm time, ts TIMESTAMP, d3 datetime(3))");
            co_await sess.query(
                "INSERT INTO t VALUES (:d, :dt, :tm, :ts, :d3)",
                db::bind("d", db::date { 2024, 6, 1 }),
                db::bind("dt", db::datetime { 2024, 6, 1, 12, 30, 45, 123456 }),
                db::bind("tm", db::time::from_duration(std::chrono::microseconds { 3723000000LL })),
                db::bind("ts", db::datetime { 2024, 6, 2, 1, 2, 3, 0 }),
                db::bind("d3", db::datetime { 2024, 6, 3, 4, 5, 6, 7 }));

            auto r = co_await sess.query("SELECT d, dt, tm, ts, d3 FROM t");
            REQUIRE(r.column_type(0) == db::column_type::date);
            REQUIRE(r.column_type(1) == db::column_type::datetime);
            REQUIRE(r.column_type(2) == db::column_type::time);
            REQUIRE(r.column_type(3) == db::column_type::datetime);
            REQUIRE(r.column_type(4) == db::column_type::datetime);

            auto d = r[0].as_date("d");
            REQUIRE(d.has_value());
            REQUIRE(*d == db::date { 2024, 6, 1 });
            auto dt = r[0].as_datetime("dt");
            REQUIRE(dt.has_value());
            REQUIRE(*dt == db::datetime { 2024, 6, 1, 12, 30, 45, 123456 });
            auto tm = r[0].as_time("tm");
            REQUIRE(tm.has_value());
            REQUIRE(tm->to_duration() == std::chrono::microseconds { 3723000000LL });
            auto d3 = r[0].as_datetime("d3");
            REQUIRE(d3.has_value());
            REQUIRE(*d3 == db::datetime { 2024, 6, 3, 4, 5, 6, 7 });
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err, "");
}

TEST_CASE("db(sqlite/bug): multi-statement SQL executes every statement", "[db][sqlite][bug]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            // MySQL（multi_queries）与 ODBC（SQLExecDirect）都会执行多条语句，SQLite 应保持一致；
            // 当前实现只 prepare/执行第一条，其余被静默丢弃
            co_await sess.query("CREATE TABLE m (x INTEGER); INSERT INTO m VALUES (42); INSERT INTO m VALUES (43)");
            auto r = co_await sess.query("SELECT COUNT(*) AS n FROM m");
            REQUIRE(*r[0].as_int64("n") == 2);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err, "");
}

TEST_CASE("db(sqlite/bug): last_insert_id does not leak into SELECT results", "[db][sqlite][bug]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY)");
            auto ins = co_await sess.query("INSERT INTO t VALUES (7)");
            REQUIRE(ins.last_insert_id() == 7);

            // 与 MySQL/ODBC 一致：SELECT 结果集不携带插入信息，不应泄漏上一条 INSERT 的 rowid
            auto sel = co_await sess.query("SELECT * FROM t");
            REQUIRE(sel.last_insert_id() == 0);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err, "");
}

TEST_CASE("db(sqlite/bug): uint64 binding above INT64_MAX fails instead of wrapping",
          "[db][sqlite][bug]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            // uint64_t > INT64_MAX 不能 static_cast 成 sqlite3_int64（回绕成负数，读回 out-of-range）；
            // 应与 MySQL 后端语义一致：要么可逆存储，要么明确失败，而非静默损坏。
            auto const max_u64 = std::numeric_limits<uint64_t>::max();
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind("a", max_u64).execute(),
                              db::db_exception);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err, "");
}

TEST_CASE("db(sqlite/bug): non-DML statements report zero affected rows", "[db][sqlite][bug]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE a (id INTEGER PRIMARY KEY)");
            auto ins = co_await sess.query("INSERT INTO a VALUES (1),(2),(3)");
            REQUIRE(ins.affected_rows() == 3);

            // CREATE TABLE 是 DDL：sqlite3_changes 只统计 INSERT/UPDATE/DELETE 且不被 DDL 更新，
            // 当前实现会泄漏上一条 INSERT 的 affected=3。
            auto ddl = co_await sess.query("CREATE TABLE b (x INTEGER)");
            REQUIRE(ddl.affected_rows() == 0);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err, "");
}

TEST_CASE("db(sqlite/bug): ping reports a corrupted database file as dead", "[db][sqlite][bug]")
{
    auto path = (std::filesystem::temp_directory_path() / "httplib_bug_ping.db").string();
    std::error_code ec;
    std::filesystem::remove(path, ec);

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=" + path);
            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY)");
            co_await sess.query("INSERT INTO t VALUES (1)");
            REQUIRE(co_await sess.ping());

            // 原地损坏数据库文件：后续 SQL 应报错，ping 应报告失效
            // （当前实现只判 db_ != nullptr，不执行任何 SQL，连接池健康检查形同虚设）
            {
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                f.write("THIS_IS_NOT_A_SQLITE_DATABASE_FILE", 34);
            }

            bool query_failed = false;
            try
            {
                co_await sess.query("SELECT COUNT(*) AS n FROM t");
            }
            catch (db::db_exception const&)
            {
                query_failed = true;
            }
            REQUIRE(query_failed);
            REQUIRE_FALSE(co_await sess.ping());
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    std::filesystem::remove(path, ec);
    rethrow_or_skip(err, "");
}

TEST_CASE("db(sqlite/bug): parameterized multi-statement SQL is rejected", "[db][sqlite][bug]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE m (x INTEGER)");

            // 带参数的多语句：prepare 只准备第一条，后续语句会被静默丢弃；
            // 修复后应显式报错而非丢数据。
            REQUIRE_THROWS_AS(co_await sess.stmt("INSERT INTO m VALUES (:a); INSERT INTO m VALUES (:b)")
                                  .bind("a", 1)
                                  .bind("b", 2)
                                  .execute(),
                              db::db_exception);

            // 纯文本多语句仍完整执行（不受参数化检测影响）
            co_await sess.query("INSERT INTO m VALUES (3); INSERT INTO m VALUES (4)");
            auto r = co_await sess.query("SELECT COUNT(*) AS n FROM m");
            REQUIRE(*r[0].as_int64("n") == 2);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err, "");
}

// ---------------------------------------------------------------------------
// ODBC (SQL Server)
// ---------------------------------------------------------------------------

namespace
{
    db::odbc_config
    odbc_bug_config()
    {
        db::odbc_config cfg;
        cfg.connection_string
            = "Driver={ODBC Driver 17 for SQL Server};Server=localhost;Database=master;Trusted_Connection=yes;";
        return cfg;
    }
} // namespace

TEST_CASE("db(odbc/bug): DATETIMEOFFSET column maps to absolute timestamp", "[db][odbc][bug][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_bug_config().to_connection_string());
            co_await sess.query("IF OBJECT_ID('httplib_odbc_bug_dto', 'U') IS NOT NULL DROP TABLE httplib_odbc_bug_dto");
            co_await sess.query("CREATE TABLE httplib_odbc_bug_dto (v DATETIMEOFFSET(6) NOT NULL)");

            // 绑定绝对时间点（UTC）→ 存 DATETIMEOFFSET +00:00 → 读回绝对时间点（固定，不随会话时区漂移）
            auto tp = std::chrono::sys_days(std::chrono::year(2024) / std::chrono::month(6) / std::chrono::day(1))
                      + std::chrono::hours(12) + std::chrono::minutes(34) + std::chrono::seconds(56)
                      + std::chrono::milliseconds(789);
            co_await sess.query("INSERT INTO httplib_odbc_bug_dto VALUES (:v)", db::bind("v", tp));

            // 字面量带偏移 +05:30 → 读回换算成 UTC 时间点 07:04:56.789
            co_await sess.query(
                "INSERT INTO httplib_odbc_bug_dto VALUES ('2024-06-01 12:34:56.789000 +05:30')");

            auto r = co_await sess.query("SELECT TOP 2 v FROM httplib_odbc_bug_dto ORDER BY v");
            REQUIRE(r.column_count() == 1);
            REQUIRE(r.column_type(0) == db::column_type::timestamp);

            // 顺序：字面量（UTC 07:04:56.789）< 绑定 tp（UTC 12:34:56.789）
            auto got0 = r[0].as_timestamp("v");
            REQUIRE(got0.has_value());
            auto expected_lit
                = std::chrono::sys_days(std::chrono::year(2024) / std::chrono::month(6) / std::chrono::day(1))
                  + std::chrono::hours(7) + std::chrono::minutes(4) + std::chrono::seconds(56)
                  + std::chrono::milliseconds(789);
            REQUIRE(*got0 == expected_lit);
            auto got1 = r[1].as_timestamp("v");
            REQUIRE(got1.has_value());
            REQUIRE(*got1 == tp);

            co_await sess.query("DROP TABLE httplib_odbc_bug_dto");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err, "odbc connect");
}

TEST_CASE("db(odbc/bug): uint64 binding above INT64_MAX fails explicitly",
          "[db][odbc][bug][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_bug_config().to_connection_string());

            // SQL Server BIGINT 有符号：绑定 > INT64_MAX 的 uint64 应显式报错（与 SQLite 后端一致），
            // 而非静默交给驱动报 22003。
            auto const max_u64 = std::numeric_limits<uint64_t>::max();
            REQUIRE_THROWS_WITH(co_await sess.stmt("SELECT CAST(:a AS BIGINT) AS x").bind("a", max_u64).execute(),
                                Catch::Matchers::ContainsSubstring("out of range"));
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err, "odbc connect");
}

TEST_CASE("db(odbc/bug): column name longer than 1023 chars does not corrupt metadata",
          "[db][odbc][bug][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_bug_config().to_connection_string());

            // 无别名的长表达式：SQL Server 对无别名表达式列返回空列名（不返回表达式文本，
            // 与 MySQL 不同）。关键是 >1024 字符的表达式不影响元数据读取：
            // 后端须动态分配列名缓冲（修复前 1024 字节固定缓冲 + std::string(name, name_len) 会越界读）。
            std::string expr = "'aa'";
            for (int i = 1; i < 220; ++i)
            {
                expr += " + 'aa'";
            }
            REQUIRE(expr.size() > 1024);

            auto r = co_await sess.query("SELECT " + expr);
            REQUIRE(r.column_count() == 1);
            REQUIRE(r.column_name(0).empty());

            // 值本身不受影响：220 个 'aa' 拼接 = 440 个 'a'
            auto v = r[0].as_string(0);
            REQUIRE(v.has_value());
            REQUIRE(*v == std::string(440, 'a'));
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err, "odbc connect");
}
#endif // HTTPLIB_ENABLED_DATABASE