#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/db/binder.hpp"
#include "httplib/db/exception.hpp"
#include "httplib/db/session.hpp"
#include "httplib/db/temporal.hpp"
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

using Catch::Approx;
using namespace std::chrono_literals;

namespace db = httplib::db;
namespace net = httplib::net;

namespace
{
    // 本地 SQL Server（通过 ODBC Driver 17）测试库；连接失败时用例 SKIP。
    db::odbc_config
    odbc_test_config()
    {
        db::odbc_config cfg;
        cfg.connection_string
            = "Driver={ODBC Driver 17 for SQL Server};Server=localhost;Database=master;Trusted_Connection=yes;";
        return cfg;
    }

    void
    rethrow_or_skip(std::exception_ptr err)
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
            if (std::string(ex.what()).find("odbc connect") != std::string::npos)
            {
                SKIP("no local SQL Server ODBC data source");
            }
            throw;
        }
    }

    net::awaitable<void>
    setup_database(db::session& sess)
    {
        co_await sess.query("IF DB_ID('httplib_odbc_test') IS NULL CREATE DATABASE httplib_odbc_test");
        co_await sess.query("USE httplib_odbc_test");
        co_await sess.query("IF OBJECT_ID('httplib_odbc_t', 'U') IS NOT NULL DROP TABLE httplib_odbc_t");
        co_await sess.query(
            "CREATE TABLE httplib_odbc_t (id INT IDENTITY(1,1) PRIMARY KEY, a BIGINT NULL, b VARCHAR(200) NULL, "
            "c DECIMAL(18,4) NULL, d VARBINARY(64) NULL, e DATE NULL, f DATETIME2(6) NULL, g TIME(6) NULL, "
            "h DATETIMEOFFSET(6) NULL)");
        co_await sess.query("IF OBJECT_ID('httplib_odbc_i', 'U') IS NOT NULL DROP TABLE httplib_odbc_i");
        co_await sess.query("CREATE TABLE httplib_odbc_i (id INT IDENTITY(1,1) PRIMARY KEY, v INT NOT NULL)");
    }
} // namespace

TEST_CASE("db(odbc): query and bindings", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());
            co_await setup_database(sess);

            // 纯文本查询
            auto one = co_await sess.query("SELECT 1 AS x, 'hi' AS s, NULL AS n");
            REQUIRE(one.row_count() == 1);
            REQUIRE(*one[0].as_int64("x") == 1);
            REQUIRE(*one[0].as_string("s") == "hi");
            REQUIRE(!one[0].as_int64("n").has_value());

            // 命名标量绑定（各类型）
            auto ins = co_await sess.query(
                "INSERT INTO httplib_odbc_t (a, b, c, d, e, f, g) VALUES (:a, :b, :c, :d, :e, :f, :g)",
                db::bind("a", int64_t { -42 }),
                db::bind("b", std::string("hello odbc")),
                db::bind("c", std::string("123.4567")),
                db::bind("d", std::span<std::byte const> { reinterpret_cast<std::byte const*>("blob123"), 7 }),
                db::bind("e", db::date { 2024, 3, 5 }),
                db::bind("f", db::datetime { 2024, 3, 5, 10, 20, 30, 600000 }),
                db::bind("g", db::time { 10, 20, 30, 0 }));
            REQUIRE(ins.affected_rows() == 1);

            auto r = co_await sess.query("SELECT TOP 1 a, b, c, d, e, f, g FROM httplib_odbc_t ORDER BY id DESC");
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_int64("a") == -42);
            REQUIRE(*r[0].as_string("b") == "hello odbc");
            REQUIRE(Approx(*r[0].as_double("c")) == 123.4567);
            auto blob = r[0].as_blob("d");
            REQUIRE(blob.has_value());
            REQUIRE(blob->size() == 7);
            REQUIRE(std::string(reinterpret_cast<char const*>(blob->data()), blob->size()) == "blob123");
            REQUIRE(*r[0].as_date("e") == db::date { 2024, 3, 5 });
            REQUIRE(*r[0].as_datetime("f") == db::datetime { 2024, 3, 5, 10, 20, 30, 600000 });
            REQUIRE(*r[0].as_time("g") == db::time { 10, 20, 30, 0 });

            // uint64 / double / bool 绑定
            co_await sess.query("INSERT INTO httplib_odbc_t (a, c) VALUES (:a, :c)",
                                db::bind("a", uint64_t { 999 }),
                                db::bind("c", std::string("1.5")));
            auto r2 = co_await sess.query("SELECT a, c FROM httplib_odbc_t WHERE a = 999");
            REQUIRE(*r2[0].as_uint64("a") == 999);
            REQUIRE(Approx(*r2[0].as_double("c")) == 1.5);

            // NULL 绑定（optional nullopt）
            co_await sess.query("INSERT INTO httplib_odbc_t (a, b) VALUES (:a, :b)",
                                db::bind("a", std::optional<int64_t> {}),
                                db::bind("b", std::optional<std::string> { std::string("nn") }));
            auto r3 = co_await sess.query("SELECT a, b FROM httplib_odbc_t WHERE b = 'nn'");
            REQUIRE(!r3[0].as_int64("a").has_value());
            REQUIRE(*r3[0].as_string("b") == "nn");

            // 位置绑定
            auto r4 = co_await sess.query("SELECT :x + :y AS s", db::bind(40), db::bind(2));
            REQUIRE(*r4[0].as_int64("s") == 42);

            // 大 blob（>4096B，触发 read_blob 的 SQLGetData 分块读取路径）
            co_await sess.query(
                "IF OBJECT_ID('httplib_odbc_bigblob', 'U') IS NOT NULL DROP TABLE httplib_odbc_bigblob");
            co_await sess.query(
                "CREATE TABLE httplib_odbc_bigblob (id INT IDENTITY(1,1) PRIMARY KEY, b VARBINARY(MAX) NOT NULL)");
            std::vector<std::byte> big(10000);
            for (size_t i = 0; i < big.size(); ++i)
            {
                big[i] = std::byte { static_cast<uint8_t>(i * 31) };
            }
            co_await sess.query("INSERT INTO httplib_odbc_bigblob (b) VALUES (:b)",
                                db::bind("b", std::span<std::byte const>(big)));
            auto rbig = co_await sess.query("SELECT TOP 1 b FROM httplib_odbc_bigblob ORDER BY id DESC");
            REQUIRE(rbig.row_count() == 1);
            auto got = rbig[0].as_blob("b");
            REQUIRE(got.has_value());
            REQUIRE(got->size() == big.size());
            REQUIRE(std::equal(big.begin(), big.end(), got->begin(), got->end()));

            // datetime（墙上时钟）绑定：往返一致
            co_await sess.query("INSERT INTO httplib_odbc_t (f) VALUES (:f)",
                                db::bind("f", db::datetime { 2024, 3, 5, 10, 20, 30, 600000 }));
            auto r5 = co_await sess.query(
                "SELECT f FROM httplib_odbc_t WHERE f >= '2000-01-01' AND f IS NOT NULL ORDER BY id DESC");
            REQUIRE(r5.row_count() >= 1);
            REQUIRE(*r5[0].as_datetime("f") == db::datetime { 2024, 3, 5, 10, 20, 30, 600000 });

            // timestamp（绝对时间点）绑定：SQL Server DATETIMEOFFSET 按 UTC 存，读回还原绝对时间点
            auto tp = std::chrono::system_clock::time_point { std::chrono::milliseconds { 1709629230000 } };
            co_await sess.query("INSERT INTO httplib_odbc_t (h) VALUES (:h)", db::bind("h", tp));
            auto r6 = co_await sess.query("SELECT TOP 1 h FROM httplib_odbc_t WHERE h IS NOT NULL ORDER BY id DESC");
            REQUIRE(r6.row_count() == 1);
            REQUIRE(r6.column_type(0) == db::column_type::timestamp);
            REQUIRE(*r6[0].as_timestamp("h") == tp);
            // as_datetime 读 timestamp 字段走 from_local_time_point（本地墙上时钟），再转回时间点应还原
            REQUIRE(r6[0].as_datetime("h")->to_local_time_point() == tp);

            // prepared_statement：命名 + 位置 + into
            co_await sess.stmt("INSERT INTO httplib_odbc_i (v) VALUES (:v)").execute(db::bind("v", 7));
            auto ps
                = co_await sess.stmt("SELECT COUNT(*) AS c FROM httplib_odbc_i WHERE v = :v").execute(db::bind("v", 7));
            REQUIRE(*ps[0].as_int64("c") == 1);

            std::optional<int64_t> into_v;
            co_await sess.stmt("SELECT :a AS x").execute(db::bind("a", 42), db::into(into_v, 0));
            REQUIRE(*into_v == 42);

            // 未绑定命名参数 → 抛
            REQUIRE_THROWS_AS(co_await sess.query("SELECT :nope AS x"), std::runtime_error);
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :nope AS x").execute(db::bind("a", 1)), std::runtime_error);

            // 非法 SQL → db_exception
            REQUIRE_THROWS_AS(co_await sess.query("SELECT * FROM no_such_table_xyz"), db::db_exception);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err);
}

TEST_CASE("db(odbc): transaction", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());
            co_await setup_database(sess);

            co_await sess.begin_transaction();
            co_await sess.query("INSERT INTO httplib_odbc_i (v) VALUES (:v)", db::bind("v", 1));
            co_await sess.rollback();
            auto after_rb = co_await sess.query("SELECT COUNT(*) AS c FROM httplib_odbc_i");
            REQUIRE(*after_rb[0].as_int64("c") == 0);

            co_await sess.begin_transaction();
            co_await sess.query("INSERT INTO httplib_odbc_i (v) VALUES (:v)", db::bind("v", 2));
            co_await sess.commit();
            auto after_cm = co_await sess.query("SELECT COUNT(*) AS c FROM httplib_odbc_i");
            REQUIRE(*after_cm[0].as_int64("c") == 1);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err);
}

TEST_CASE("db(odbc): reconnect", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());
            co_await setup_database(sess);

            co_await sess.query("INSERT INTO httplib_odbc_i (v) VALUES (:v)", db::bind("v", 5));
            co_await sess.reconnect();
            // ODBC 的 USE 是会话级状态，reconnect 后回到连接串默认库（master），需重新切换。
            co_await sess.query("USE httplib_odbc_test");
            auto r = co_await sess.query("SELECT COUNT(*) AS c FROM httplib_odbc_i WHERE v = 5");
            REQUIRE(*r[0].as_int64("c") == 1);

            // reconnect 后 prepared 语句仍可用（统一层重建）
            auto ps = co_await sess.stmt("SELECT :a AS x").execute(db::bind("a", 11));
            REQUIRE(*ps[0].as_int64("x") == 11);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err);
}

TEST_CASE("db(odbc): uniqueidentifier", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());
            co_await sess.query("IF OBJECT_ID('httplib_odbc_u', 'U') IS NOT NULL DROP TABLE httplib_odbc_u");
            co_await sess.query(
                "CREATE TABLE httplib_odbc_u (id INT IDENTITY(1,1) PRIMARY KEY, g UNIQUEIDENTIFIER NULL)");

            // 字符串绑定 GUID → SQL Server 隐式转换
            co_await sess.query("INSERT INTO httplib_odbc_u (g) VALUES (:g)",
                                db::bind("g", std::string("6F9619FF-8B86-D011-B42D-00C04FC964FF")));
            auto r = co_await sess.query("SELECT g FROM httplib_odbc_u");
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_string("g") == "6F9619FF-8B86-D011-B42D-00C04FC964FF");

            // NULL GUID（optional nullopt → NULL 绑定）
            co_await sess.query("INSERT INTO httplib_odbc_u (g) VALUES (:g)",
                                db::bind("g", std::optional<std::string> {}));
            auto rn = co_await sess.query("SELECT g FROM httplib_odbc_u WHERE g IS NULL");
            REQUIRE(rn.row_count() == 1);
            REQUIRE(!rn[0].as_string("g").has_value());
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err);
}

TEST_CASE("db(odbc): async notification (slow query does not block io thread)", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    std::atomic_bool query_done { false };
    bool timer_fired_while_pending = false;

    // 慢查询协程：WAITFOR DELAY 强制驱动走 SQL_STILL_EXECUTING → 事件通知 → SQLCompleteAsync。
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());
            co_await sess.query("WAITFOR DELAY '00:00:02'");
            query_done.store(true);
        },
        [&](std::exception_ptr e) { err = e; });

    // 定时器协程：300ms 后检查慢查询是否仍在执行。若 ODBC 调用阻塞了 io 线程，
    // 该定时器要等慢查询结束（2s）后才能触发，timer_fired_while_pending 为 false。
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            net::steady_timer timer(ioc.get_executor(), std::chrono::milliseconds { 300 });
            co_await timer.async_wait(net::use_awaitable);
            timer_fired_while_pending = !query_done.load();
        },
        [&](std::exception_ptr e) { err = e; });

    ioc.run();
    rethrow_or_skip(err);
    REQUIRE(timer_fired_while_pending);
}

TEST_CASE("db(odbc): multiple result sets", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());
            auto r = co_await sess.query("SELECT 1 AS a; SELECT 'x' AS b");
            REQUIRE(r.resultset_count() == 2);
            REQUIRE(*r[0].as_int64("a") == 1);
            REQUIRE(r.next_resultset());
            REQUIRE(*r[0].as_string("b") == "x");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err);
}

TEST_CASE("db(odbc): big text VARCHAR(MAX) chunked read", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());
            co_await sess.query(
                "IF OBJECT_ID('httplib_odbc_bigtext', 'U') IS NOT NULL DROP TABLE httplib_odbc_bigtext");
            co_await sess.query(
                "CREATE TABLE httplib_odbc_bigtext (id INT IDENTITY(1,1) PRIMARY KEY, t VARCHAR(MAX) NOT NULL)");

            // 12000 字节文本：触发 SQL_LONGVARCHAR 绑定 + read_text 的 SQL_SUCCESS_WITH_INFO 分块路径。
            std::string txt;
            txt.reserve(12000);
            for (int i = 0; i < 12000; ++i)
            {
                txt.push_back(static_cast<char>('a' + (i % 26)));
            }
            co_await sess.query("INSERT INTO httplib_odbc_bigtext (t) VALUES (:t)", db::bind("t", txt));
            auto r = co_await sess.query("SELECT TOP 1 t FROM httplib_odbc_bigtext ORDER BY id DESC");
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_string("t") == txt);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err);
}

TEST_CASE("db(odbc): datetimeoffset / time2 type mapping", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());
            co_await sess.query("IF OBJECT_ID('httplib_odbc_map', 'U') IS NOT NULL DROP TABLE httplib_odbc_map");
            co_await sess.query("CREATE TABLE httplib_odbc_map (id INT IDENTITY(1,1) PRIMARY KEY, "
                                "o DATETIMEOFFSET(6) NULL, t TIME(3) NULL, ts DATETIME2(7) NULL)");

            co_await sess.query("INSERT INTO httplib_odbc_map (o, t, ts) VALUES "
                                "(CAST('2024-03-05 10:20:30.123456' AS DATETIME2), "
                                "CAST('10:20:30.123' AS TIME(3)), "
                                "CAST('2024-03-05 10:20:30.1234567' AS DATETIME2(7)))");
            // DATETIMEOFFSET 列读回换算成绝对时间点（UTC）：
            // datetime2 → datetimeoffset 的隐式转换偏移固定为 +00:00，即 UTC 时间点。
            db::timestamp o_expected = db::datetime { 2024, 3, 5, 10, 20, 30, 123456 }.to_utc_time_point();
            auto r = co_await sess.query("SELECT TOP 1 o, t, ts FROM httplib_odbc_map ORDER BY id DESC");
            REQUIRE(r.row_count() == 1);
            // DATETIMEOFFSET → SQL_SS_TIMESTAMPOFFSET → timestamp
            REQUIRE(r.column_type(0) == db::column_type::timestamp);
            REQUIRE(*r[0].as_timestamp("o") == o_expected);
            // TIME(3) → SQL_SS_TIME2 → time（小数秒须保留，修复前恒为 0）
            REQUIRE(r.column_type(1) == db::column_type::time);
            REQUIRE(*r[0].as_time("t") == db::time { 10, 20, 30, 123000 });
            // DATETIME2(7) → SQL_SS_TIMESTAMP2 → datetime
            REQUIRE(r.column_type(2) == db::column_type::datetime);
            REQUIRE(*r[0].as_datetime("ts") == db::datetime { 2024, 3, 5, 10, 20, 30, 123456 });

            // TIME 参数绑定也须保留小数秒（SQL_SS_TIME2 + SQL_C_BINARY）。
            co_await sess.query("INSERT INTO httplib_odbc_map (t) VALUES (:t)",
                                db::bind("t", db::time { 11, 22, 33, 456000 }));
            auto r2 = co_await sess.query("SELECT TOP 1 t FROM httplib_odbc_map ORDER BY id DESC");
            REQUIRE(*r2[0].as_time("t") == db::time { 11, 22, 33, 456000 });
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err);
}

TEST_CASE("db(odbc): long error message is clamped", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());

            // RAISERROR 消息上限 2047 字节；此处 1500+，足以越过 odbc_error_text 的 1024 栈缓冲。
            std::string msg;
            msg.reserve(1500);
            while (msg.size() < 1500)
            {
                msg += "httplib_odbc_longerr_";
            }

            bool threw = false;
            std::string what;
            try
            {
                co_await sess.query("RAISERROR('" + msg + "', 16, 1)");
            }
            catch (db::db_exception const& ex)
            {
                threw = true;
                what = ex.what();
            }
            REQUIRE(threw);
            // SQL Server 驱动会把诊断消息截到 1023 字节，本用例是防御性钳制守卫：
            // odbc_error_text 内部必须把 msg_len 钳到缓冲大小，否则其他驱动
            // 报告完整长度时会栈越界读，what() 也可能膨胀到 1500+ 字节。
            REQUIRE(what.size() < 1300);
            REQUIRE(what.find("httplib_odbc_longerr_") != std::string::npos);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err);
}

TEST_CASE("db(odbc): ping and close_statement lifecycle", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());
            REQUIRE(co_await sess.ping());

            // 反复创建/销毁 prepared statement，触发 close_statement 资源释放路径。
            for (int i = 0; i < 10; ++i)
            {
                auto r = co_await sess.stmt("SELECT :a AS x").execute(db::bind("a", i));
                REQUIRE(*r[0].as_int64("x") == i);
            }
            // 销毁后连接仍可正常使用，且 prepared 语句可重建。
            REQUIRE(co_await sess.ping());
            auto r = co_await sess.stmt("SELECT :a AS x").execute(db::bind("a", 99));
            REQUIRE(*r[0].as_int64("x") == 99);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err);
}
TEST_CASE("db(odbc): error paths", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "odbc", odbc_test_config().to_connection_string());
            co_await setup_database(sess);

            // 主键冲突 → SQL Server 原生错误 2627 → db_exception（含诊断文本）
            co_await sess.query("IF OBJECT_ID('httplib_odbc_pk', 'U') IS NOT NULL DROP TABLE httplib_odbc_pk");
            co_await sess.query("CREATE TABLE httplib_odbc_pk (id INT PRIMARY KEY, v INT NOT NULL)");
            co_await sess.query("INSERT INTO httplib_odbc_pk (id, v) VALUES (1, 10)");
            REQUIRE_THROWS_AS(co_await sess.query("INSERT INTO httplib_odbc_pk (id, v) VALUES (1, 20)"),
                              db::db_exception);

            // 类型转换失败 → SQL Server 原生错误 245 → db_exception
            co_await sess.query("IF OBJECT_ID('httplib_odbc_conv', 'U') IS NOT NULL DROP TABLE httplib_odbc_conv");
            co_await sess.query("CREATE TABLE httplib_odbc_conv (id INT IDENTITY(1,1) PRIMARY KEY, v INT NULL)");
            REQUIRE_THROWS_AS(co_await sess.query("INSERT INTO httplib_odbc_conv (v) VALUES (:v)",
                                                  db::bind("v", std::string("not_a_number"))),
                              db::db_exception);

            // 数值越界（超出 INT 范围）→ SQL Server 原生错误 220/2628 → db_exception
            co_await sess.query("IF OBJECT_ID('httplib_odbc_range', 'U') IS NOT NULL DROP TABLE httplib_odbc_range");
            co_await sess.query("CREATE TABLE httplib_odbc_range (id INT IDENTITY(1,1) PRIMARY KEY, v SMALLINT NULL)");
            REQUIRE_THROWS_AS(co_await sess.query("INSERT INTO httplib_odbc_range (v) VALUES (:v)",
                                                  db::bind("v", int64_t { 99999999 })),
                              db::db_exception);

            // 唯一约束冲突 → db_exception
            co_await sess.query("IF OBJECT_ID('httplib_odbc_uniq', 'U') IS NOT NULL DROP TABLE httplib_odbc_uniq");
            co_await sess.query(
                "CREATE TABLE httplib_odbc_uniq (id INT IDENTITY(1,1) PRIMARY KEY, v INT UNIQUE NOT NULL)");
            co_await sess.query("INSERT INTO httplib_odbc_uniq (v) VALUES (:v)", db::bind("v", 7));
            REQUIRE_THROWS_AS(co_await sess.query("INSERT INTO httplib_odbc_uniq (v) VALUES (:v)", db::bind("v", 7)),
                              db::db_exception);

            // 绑定位置/命名混用 → 通用层抛 runtime_error
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind(1).bind("a", 2).execute(), std::runtime_error);
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind("a", 2).bind(1).execute(), std::runtime_error);

            // 同名重复绑定 → 后者生效（不抛）
            auto dup = co_await sess.stmt("SELECT :v AS x").bind("v", 1).bind("v", 2).execute();
            REQUIRE(*dup[0].as_int64("x") == 2);

            // 位置绑定数量多于占位符 → db_exception（后端参数不匹配）
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind(1).bind(2).execute(), db::db_exception);

            // 事务内出错 → with_transaction 自动回滚，数据不落库
            REQUIRE_THROWS_AS(co_await sess.with_transaction(
                                  [&](db::session& s) -> net::awaitable<void>
                                  {
                                      co_await s.query("INSERT INTO httplib_odbc_i (v) VALUES (:v)", db::bind("v", 99));
                                      co_await s.query("INSERT INTO httplib_odbc_pk (id, v) VALUES (1, 99)");
                                  }),
                              db::db_exception);
            auto after = co_await sess.query("SELECT COUNT(*) AS c FROM httplib_odbc_i WHERE v = 99");
            REQUIRE(*after[0].as_int64("c") == 0);

            // NULL 读回：字符串/二进制 NULL 列 → as_string / as_blob 返回 nullopt
            // （NULL 绑定走 VARCHAR 类型，插不进 VARBINARY 列，故用 SQL 字面 NULL）
            co_await sess.query("INSERT INTO httplib_odbc_t (b, d) VALUES (NULL, NULL)");
            auto rn = co_await sess.query("SELECT TOP 1 b, d FROM httplib_odbc_t WHERE b IS NULL ORDER BY id DESC");
            REQUIRE(rn.row_count() == 1);
            REQUIRE(!rn[0].as_string("b").has_value());
            REQUIRE(!rn[0].as_blob("d").has_value());
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    rethrow_or_skip(err);
}
#endif // HTTPLIB_ENABLED_DATABASE