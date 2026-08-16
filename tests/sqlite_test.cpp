#ifdef HTTPLIB_ENABLED_DATABASE
#include "db/render.hpp"
#include "httplib/db/binder.hpp"
#include "httplib/db/connection_pool.hpp"
#include "httplib/db/exception.hpp"
#include "httplib/db/extractor.hpp"
#include "httplib/db/session.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

namespace db = httplib::db;
namespace net = httplib::net;

TEST_CASE("db(sqlite): connect via connection string", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
            co_await sess.query("INSERT INTO t VALUES (1, 'x')");
            auto r = co_await sess.query("SELECT COUNT(*) AS n FROM t");
            REQUIRE(*r[0].as_int64("n") == 1);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): connection string aliases", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            // path= 与 db= 别名等价
            {
                auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "path=:memory:");
                co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
                auto r = co_await sess.query("SELECT COUNT(*) AS n FROM t");
                REQUIRE(*r[0].as_int64("n") == 0);
            }
            // 空连接串 → 默认 :memory:
            {
                auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "");
                co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
                co_await sess.query("INSERT INTO t VALUES (1, 'x')");
                auto r = co_await sess.query("SELECT COUNT(*) AS n FROM t");
                REQUIRE(*r[0].as_int64("n") == 1);
            }
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): basic round-trip", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), db::sqlite_config { ":memory:" });

            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, n INTEGER)");

            co_await sess.query("INSERT INTO t VALUES (:id, :name, :n)",
                                db::bind("id", 1),
                                db::bind("name", std::string("alice")),
                                db::bind("n", 42));

            co_await sess.query("INSERT INTO t VALUES (:id, :name, :n)",
                                db::bind("id", 2),
                                db::bind("name", std::string("bob")),
                                db::bind("n", nullptr));

            auto r = co_await sess.query("SELECT id, name, n FROM t ORDER BY id");
            REQUIRE(r.row_count() == 2);
            REQUIRE(r.column_count() == 3);
            REQUIRE(*r[0].as_int64("id") == 1);
            REQUIRE(*r[0].as_string("name") == "alice");
            REQUIRE(*r[0].as_int64("n") == 42);
            REQUIRE(r[1].is_null("n"));
            REQUIRE(*r[1].as_string("name") == "bob");

            std::vector<std::string> names;
            co_await sess.query("SELECT name FROM t ORDER BY id", db::into(names));
            REQUIRE(names == std::vector<std::string> { "alice", "bob" });
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): connection pool round-trip", "[db][sqlite]")
{
    auto path = (std::filesystem::temp_directory_path() / "httplib_pool_test.db").string();
    std::error_code ec;
    std::filesystem::remove(path, ec);

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::pool_params p;
            p.min_connections = 1;
            p.max_connections = 2;
            db::connection_pool pool = db::make_pool(ioc.get_executor(), p, "sqlite", "db=" + path);
            pool.start();

            auto h1 = co_await pool.async_acquire();
            co_await h1->query("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");
            co_await h1->query("INSERT INTO t VALUES (1, 'x')");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::filesystem::remove(path, ec);
        std::rethrow_exception(err);
    }
    std::filesystem::remove(path, ec);
}

TEST_CASE("db(sqlite): multi-row fetch accuracy", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), db::sqlite_config { ":memory:" });
            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, n INTEGER, d REAL, note TEXT)");
            co_await sess.query(
                "INSERT INTO t VALUES (1,'alpha',10,1.5,'x'),(2,'beta',20,2.5,NULL),(3,'gamma',30,3.5,'z')");

            auto r = co_await sess.query("SELECT id, name, n, d, note FROM t ORDER BY id");
            REQUIRE(r.row_count() == 3);
            REQUIRE(r.column_count() == 5);
            REQUIRE(*r[0].as_int64("id") == 1);
            REQUIRE(*r[0].as_string("name") == "alpha");
            REQUIRE(*r[0].as_int64("n") == 10);
            REQUIRE(*r[0].as_double("d") == 1.5);
            REQUIRE(*r[0].as_string("note") == "x");
            REQUIRE(*r[1].as_string("name") == "beta");
            REQUIRE(*r[1].as_int64("n") == 20);
            REQUIRE(r[1].is_null("note"));
            REQUIRE(*r[2].as_string("name") == "gamma");
            REQUIRE(*r[2].as_double("d") == 3.5);

            // 1000 行批量写入后逐行核对
            co_await sess.query("CREATE TABLE t2 (id INTEGER PRIMARY KEY, v TEXT)");
            auto stmt = sess.stmt("INSERT INTO t2 VALUES (:id, :v)");
            for (int i = 0; i < 1000; ++i)
            {
                co_await stmt.bind("id", i).bind("v", "row-" + std::to_string(i)).execute();
            }
            auto r2 = co_await sess.query("SELECT id, v FROM t2 ORDER BY id");
            REQUIRE(r2.row_count() == 1000);
            int64_t sum = 0;
            for (size_t i = 0; i < r2.row_count(); ++i)
            {
                REQUIRE(*r2[i].as_int64("id") == static_cast<int64_t>(i));
                REQUIRE(*r2[i].as_string("v") == "row-" + std::to_string(i));
                sum += *r2[i].as_int64("id");
            }
            REQUIRE(sum == 999 * 1000 / 2);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): prepared statement bind types", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), db::sqlite_config { ":memory:" });
            co_await sess.query("CREATE TABLE t (u INTEGER, d REAL, b BLOB, dt TEXT, tm TEXT)");

            auto stmt = sess.stmt("INSERT INTO t VALUES (:u, :d, :b, :dt, :tm)");
            std::byte blob[] = { std::byte { 0x00 }, std::byte { 0xFF } };
            co_await stmt.bind("u", uint64_t { 7 })
                .bind("d", 3.25)
                .bind("b", std::span<std::byte const>(blob, 2))
                .bind("dt", db::datetime { 2024, 6, 1, 12, 0, 0, 0 })
                .bind("tm", db::time::from_duration(std::chrono::hours { 2 }))
                .execute();

            auto r = co_await sess.query("SELECT u, d, b, dt, tm FROM t");
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_uint64("u") == 7);
            REQUIRE(*r[0].as_double("d") == 3.25);
            REQUIRE(r[0].as_blob("b")->size() == 2);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): transactions", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");

            // 手动 begin/rollback：数据不落库
            co_await sess.begin_transaction();
            co_await sess.query("INSERT INTO t VALUES (1, 'a')");
            REQUIRE(sess.in_transaction());
            co_await sess.rollback();
            REQUIRE_FALSE(sess.in_transaction());
            auto r = co_await sess.query("SELECT COUNT(*) AS n FROM t");
            REQUIRE(*r[0].as_int64("n") == 0);

            // 手动 begin/commit：数据落库
            co_await sess.begin_transaction();
            co_await sess.query("INSERT INTO t VALUES (1, 'a')");
            co_await sess.commit();
            REQUIRE_FALSE(sess.in_transaction());
            auto r2 = co_await sess.query("SELECT COUNT(*) AS n FROM t");
            REQUIRE(*r2[0].as_int64("n") == 1);

            // with_transaction 成功 → 提交
            co_await sess.with_transaction([](db::session& s) -> net::awaitable<void>
                                           { co_await s.query("INSERT INTO t VALUES (2, 'b')"); });
            auto r3 = co_await sess.query("SELECT COUNT(*) AS n FROM t");
            REQUIRE(*r3[0].as_int64("n") == 2);

            // with_transaction 抛异常 → 回滚
            REQUIRE_THROWS_AS(co_await sess.with_transaction(
                                  [](db::session& s) -> net::awaitable<void>
                                  {
                                      co_await s.query("INSERT INTO t VALUES (3, 'c')");
                                      throw std::runtime_error("boom");
                                  }),
                              std::runtime_error);
            auto r4 = co_await sess.query("SELECT COUNT(*) AS n FROM t");
            REQUIRE(*r4[0].as_int64("n") == 2);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): file persistence and reconnect", "[db][sqlite]")
{
    auto path = (std::filesystem::temp_directory_path() / "httplib_persist_test.db").string();
    std::error_code ec;
    std::filesystem::remove(path, ec);

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            {
                auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=" + path);
                co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
                co_await sess.query("INSERT INTO t VALUES (1, 'persisted')");
                REQUIRE(co_await sess.ping());
            }
            // 新会话读同一文件；reconnect 后数据仍在
            {
                auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=" + path);
                auto r = co_await sess.query("SELECT v FROM t WHERE id = 1");
                REQUIRE(*r[0].as_string("v") == "persisted");
                co_await sess.reconnect();
                auto r2 = co_await sess.query("SELECT COUNT(*) AS n FROM t");
                REQUIRE(*r2[0].as_int64("n") == 1);
            }
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::filesystem::remove(path, ec);
        std::rethrow_exception(err);
    }
    std::filesystem::remove(path, ec);
}

TEST_CASE("db(sqlite): temporal types round-trip", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (d DATE, dt DATETIME, tm TIME, ts TIMESTAMP)");
            co_await sess.query(
                "INSERT INTO t VALUES (:d, :dt, :tm, :ts)",
                db::bind("d", db::date { 2024, 6, 1 }),
                db::bind("dt", db::datetime { 2024, 6, 1, 12, 30, 45, 123456 }),
                db::bind("tm", db::time::from_duration(std::chrono::microseconds { 3723000000LL })),
                db::bind("ts",
                         std::chrono::sys_days(std::chrono::year(2024) / std::chrono::month(6) / std::chrono::day(1))
                             + std::chrono::hours(12)));

            auto r = co_await sess.query("SELECT d, dt, tm, ts FROM t");
            REQUIRE(r.column_count() == 4);
            REQUIRE(r.column_type(0) == db::column_type::date);
            REQUIRE(r.column_type(1) == db::column_type::datetime);
            REQUIRE(r.column_type(2) == db::column_type::time);
            REQUIRE(r.column_type(3) == db::column_type::datetime); // sqlite 后端把 TIMESTAMP 声明映射为 datetime

            auto d = *r[0].as_date("d");
            REQUIRE(d.year == 2024);
            REQUIRE(d.month == 6);
            REQUIRE(d.day == 1);

            auto dt = *r[0].as_datetime("dt");
            REQUIRE(dt.year == 2024);
            REQUIRE(dt.month == 6);
            REQUIRE(dt.day == 1);
            REQUIRE(dt.hour == 12);
            REQUIRE(dt.minute == 30);
            REQUIRE(dt.second == 45);
            REQUIRE(dt.microsecond == 123456);

            auto tm = *r[0].as_time("tm");
            REQUIRE(tm.to_duration() == std::chrono::microseconds { 3723000000LL });

            // time_point：SQLite 无时区，按 UTC 存文本，往返一致
            auto ts = *r[0].as_timestamp("ts");
            std::chrono::system_clock::time_point expected
                = std::chrono::sys_days(std::chrono::year(2024) / std::chrono::month(6) / std::chrono::day(1))
                  + std::chrono::hours(12);
            REQUIRE(ts == expected);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): integer edge values", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (a INTEGER, b INTEGER)");
            int64_t const neg = -9007199254740993LL;     // 超出 double 精确范围
            uint64_t const big = 9223372036854775807ULL; // INT64_MAX
            co_await sess.query("INSERT INTO t VALUES (:a, :b)", db::bind("a", neg), db::bind("b", big));

            auto r = co_await sess.query("SELECT a, b FROM t");
            REQUIRE(*r[0].as_int64("a") == neg);
            REQUIRE(*r[0].as_uint64("b") == big);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): affected rows", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
            auto ins = co_await sess.query("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");
            REQUIRE(ins.affected_rows() == 3);
            auto upd = co_await sess.query("UPDATE t SET v = 'x' WHERE id <= 2");
            REQUIRE(upd.affected_rows() == 2);
            auto del = co_await sess.query("DELETE FROM t WHERE id > 2");
            REQUIRE(del.affected_rows() == 1);
            auto sel = co_await sess.query("SELECT * FROM t");
            REQUIRE(sel.affected_rows() == 0);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): empty resultset", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
            auto r = co_await sess.query("SELECT * FROM t WHERE id = 999");
            REQUIRE(r.empty());
            REQUIRE(r.row_count() == 0);
            REQUIRE(r.column_count() == 2);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): error paths", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");

            // 非法 SQL → db_exception
            REQUIRE_THROWS_AS(co_await sess.query("SELECT FROM nowhere"), db::db_exception);

            // 未绑定/未知命名参数（统一层抛 std::runtime_error）
            REQUIRE_THROWS_AS(co_await sess.query("SELECT :a AS x", db::bind("nope", 1)), std::runtime_error);
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind("b", 42).execute(), std::runtime_error);
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").execute(), std::runtime_error);

            // 主键冲突 → SQLITE_CONSTRAINT → db_exception
            co_await sess.query("CREATE TABLE t_pk (id INTEGER PRIMARY KEY, v INTEGER NOT NULL)");
            co_await sess.query("INSERT INTO t_pk VALUES (1, 10)");
            REQUIRE_THROWS_AS(co_await sess.query("INSERT INTO t_pk VALUES (1, 20)"), db::db_exception);

            // 唯一约束冲突 → db_exception
            co_await sess.query("CREATE TABLE t_uq (id INTEGER PRIMARY KEY, v INTEGER UNIQUE NOT NULL)");
            co_await sess.query("INSERT INTO t_uq VALUES (1, 7)");
            REQUIRE_THROWS_AS(co_await sess.query("INSERT INTO t_uq VALUES (2, 7)"), db::db_exception);

            // NOT NULL 约束冲突（sqlite 动态类型，无类型转换/越界错误）→ db_exception
            co_await sess.query("CREATE TABLE t_nn (id INTEGER PRIMARY KEY, v INTEGER NOT NULL)");
            REQUIRE_THROWS_AS(co_await sess.query("INSERT INTO t_nn (id, v) VALUES (1, NULL)"), db::db_exception);

            // 绑定位置/命名混用 → 通用层抛 runtime_error
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind(1).bind("a", 2).execute(), std::runtime_error);
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind("a", 2).bind(1).execute(), std::runtime_error);

            // 同名重复绑定 → 后者生效（不抛）
            auto dup = co_await sess.stmt("SELECT :v AS x").bind("v", 1).bind("v", 2).execute();
            REQUIRE(*dup[0].as_int64("x") == 2);

            // 位置绑定数量多于占位符 → db_exception（后端参数不匹配）
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind(1).bind(2).execute(), db::db_exception);

            // 事务内出错（SQL 层错误）→ with_transaction 自动回滚，数据不落库
            REQUIRE_THROWS_AS(co_await sess.with_transaction(
                                  [&](db::session& s) -> net::awaitable<void>
                                  {
                                      co_await s.query("INSERT INTO t_pk VALUES (2, 99)");
                                      co_await s.query("INSERT INTO t_pk VALUES (1, 100)");
                                  }),
                              db::db_exception);
            auto after = co_await sess.query("SELECT COUNT(*) AS c FROM t_pk WHERE v = 99");
            REQUIRE(*after[0].as_int64("c") == 0);

            // NULL 读回：字符串/二进制 NULL 列 → as_string / as_blob 返回 nullopt
            co_await sess.query("CREATE TABLE t_null (s TEXT NULL, b BLOB NULL)");
            co_await sess.query("INSERT INTO t_null VALUES (NULL, NULL)");
            auto rn = co_await sess.query("SELECT s, b FROM t_null LIMIT 1");
            REQUIRE(rn.row_count() == 1);
            REQUIRE(!rn[0].as_string("s").has_value());
            REQUIRE(!rn[0].as_blob("b").has_value());

            // 正常绑定不受影响
            auto r = co_await sess.stmt("SELECT :a AS x").bind("a", 42).execute();
            REQUIRE(*r[0].as_int64("x") == 42);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): query logger", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            std::vector<db::query_log_entry> logs;
            sess.set_query_logger([&](db::query_log_entry const& e) { logs.push_back(e); });

            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
            co_await sess.query("INSERT INTO t VALUES (:id, :v)", db::bind("id", 1), db::bind("v", std::string("x")));
            co_await sess.query("SELECT COUNT(*) AS n FROM t");

            REQUIRE(logs.size() == 3);
            REQUIRE_FALSE(logs[0].is_parameterized); // CREATE TABLE
            REQUIRE(logs[0].affected_rows == 0);

            REQUIRE(logs[1].is_parameterized); // INSERT 带参数
            REQUIRE(logs[1].sql.find(":id") != std::string::npos);
            REQUIRE(logs[1].affected_rows == 1);
            REQUIRE(logs[1].row_count == 0);

            REQUIRE_FALSE(logs[2].is_parameterized); // SELECT
            REQUIRE(logs[2].row_count == 1);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): scalar accessors and column types", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (i INTEGER, r REAL, s TEXT, b BLOB, n TEXT)");

            std::byte blob[] = { std::byte { 0x00 }, std::byte { 0xFF }, std::byte { 0x7F } };
            co_await sess.query("INSERT INTO t VALUES (:i, :r, :s, :b, :n)",
                                db::bind("i", int64_t { 1 }),
                                db::bind("r", 3.5),
                                db::bind("s", std::string("{\"k\":42}")),
                                db::bind("b", std::span<std::byte const>(blob, 3)),
                                db::bind("n", nullptr));

            auto r = co_await sess.query("SELECT i, r, s, b, n FROM t");
            REQUIRE(r.row_count() == 1);

            // 普通列声明类型不在 DATE/DATETIME/TIME 之列 → unknown
            REQUIRE(r.column_type(0) == db::column_type::unknown); // INTEGER
            REQUIRE(r.column_type(1) == db::column_type::unknown); // REAL
            REQUIRE(r.column_type(2) == db::column_type::unknown); // TEXT
            REQUIRE(r.column_type(3) == db::column_type::unknown); // BLOB
            REQUIRE(r.column_type(4) == db::column_type::unknown); // NULL

            // 各类标量访问器
            REQUIRE(*r[0].as_bool("i")); // INTEGER 1 → true
            REQUIRE(*r[0].as_float("r") == 3.5f);
            REQUIRE(*r[0].as_string("s") == "{\"k\":42}");
            REQUIRE(r[0].get<int>("i") == 1);
            REQUIRE(r[0].get<unsigned short>("i") == 1);
            REQUIRE(r[0].get<std::string>("s") == "{\"k\":42}");
            REQUIRE(r[0].is_null("n"));
            REQUIRE_FALSE(r[0].as_string("n").has_value());

            // BLOB 内容往返一致
            auto b = r[0].as_blob("b");
            REQUIRE(b.has_value());
            REQUIRE(b->size() == 3);
            REQUIRE((*b)[0] == std::byte { 0x00 });
            REQUIRE((*b)[1] == std::byte { 0xFF });
            REQUIRE((*b)[2] == std::byte { 0x7F });

            // JSON 文本 → as_json
            auto j = r[0].as_json("s");
            REQUIRE(j.has_value());
            REQUIRE(j->at("k").as_int64() == 42);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): prepared statement binds", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");

            // 位置绑定
            auto r = co_await sess.stmt("SELECT :a + :b AS x").bind(1).bind(2).execute();
            REQUIRE(*r[0].as_int64("x") == 3);

            // 位置绑定 + into
            std::optional<int64_t> v;
            co_await sess.stmt("SELECT :a AS x").bind(42).execute(db::into(v, 0));
            REQUIRE(*v == 42);

            // 命名绑定重绑：同一 stmt 换值
            auto stmt = sess.stmt("SELECT :v AS x");
            auto r1 = co_await stmt.bind("v", 10).execute();
            auto r2 = co_await stmt.bind("v", 20).execute();
            REQUIRE(*r1[0].as_int64("x") == 10);
            REQUIRE(*r2[0].as_int64("x") == 20);

            // 同名重复绑定 → 后者生效
            auto r3 = co_await sess.stmt("SELECT :v AS x").bind("v", 1).bind("v", 2).execute();
            REQUIRE(*r3[0].as_int64("x") == 2);

            // 位置/命名混用 → 通用层抛
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind(1).bind("a", 2).execute(), std::runtime_error);
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind("a", 2).bind(1).execute(), std::runtime_error);

            // 位置绑定数量多于占位符 → sqlite 层抛 db_exception
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind(1).bind(2).execute(), db::db_exception);

            // 未绑定的命名参数 → 通用层抛
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").execute(), std::runtime_error);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): into scalar types", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (a INTEGER, s TEXT, n INTEGER)");
            co_await sess.query("INSERT INTO t VALUES (1, 'x', NULL)");

            // 位置序（第一列）
            int64_t a = 0;
            co_await sess.query("SELECT a FROM t", db::into(a));
            REQUIRE(a == 1);

            // 下标
            int64_t b = 0;
            co_await sess.query("SELECT a, s FROM t", db::into(b, 0));
            REQUIRE(b == 1);

            // 列名
            std::string s;
            co_await sess.query("SELECT a, s FROM t", db::into(s, "s"));
            REQUIRE(s == "x");

            // 裸类型 + prepared stmt
            int64_t c = 0;
            co_await sess.stmt("SELECT :v AS a").bind("v", 7).execute(db::into(c, "a"));
            REQUIRE(c == 7);

            // NULL → 抛异常（与 optional 的 nullopt 语义不同）
            REQUIRE_THROWS_AS(co_await sess.query("SELECT n FROM t", db::into(a, 0)), std::runtime_error);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): bind optional syntax sugar", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, s TEXT, d REAL)");

            // 命名绑定：有值 → 值；nullopt → NULL
            co_await sess.query("INSERT INTO t VALUES (1, :a, :s, :d)",
                                db::bind("a", std::optional<int64_t> { 42 }),
                                db::bind("s", std::optional<std::string> {}),
                                db::bind("d", std::optional<double> { 3.5 }));

            // 位置绑定 + nullopt
            co_await sess.query("INSERT INTO t VALUES (2, :a, :s, :d)",
                                db::bind(std::optional<int> {}),
                                db::bind(std::optional<std::string> { std::string("y") }),
                                db::bind(std::optional<double> {}));

            auto r = co_await sess.query("SELECT a, s, d FROM t ORDER BY id");
            REQUIRE(r.row_count() == 2);
            REQUIRE(*r[0].as_int64("a") == 42);
            REQUIRE(r[0].is_null("s"));
            REQUIRE(*r[0].as_double("d") == 3.5);
            REQUIRE(r[1].is_null("a"));
            REQUIRE(*r[1].as_string("s") == "y");
            REQUIRE(r[1].is_null("d"));
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): array binds expand for INSERT and IN", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, b TEXT)");

            // 列式数组：`VALUES (:a, :b, :c)` 每个占位符是一列的数据，长度 1 的数组 = 一行多列
            co_await sess.query("INSERT INTO t (id, a, b) VALUES (:ids, :as, :bs)",
                                db::bind("ids", std::vector<int64_t> { 1 }),
                                db::bind("as", std::vector<int64_t> { 42 }),
                                db::bind("bs", std::vector<std::string> { std::string("x") }));
            co_await sess.query("INSERT INTO t (id, a, b) VALUES (:ids, :as, :bs)",
                                db::bind("ids", std::vector<int64_t> { 2 }),
                                db::bind("as", std::vector<int64_t> { 7 }),
                                db::bind("bs", std::vector<std::string> { std::string("y") }));

            // 列式数组：等长数组 = 多行批量插入
            co_await sess.query("INSERT INTO t (id, a, b) VALUES (:ids, :as, :bs)",
                                db::bind("ids", std::vector<int64_t> { 5, 6 }),
                                db::bind("as", std::vector<int64_t> { 1, 2 }),
                                db::bind("bs", std::vector<std::string> { std::string("u"), std::string("v") }));

            // IN 子句：展开为 ?,?,?
            auto r = co_await sess.query("SELECT id, a FROM t WHERE id IN (:ids) ORDER BY id",
                                         db::bind("ids", std::vector<int64_t> { 1, 2 }));
            REQUIRE(r.row_count() == 2);
            REQUIRE(*r[0].as_int64("id") == 1);
            REQUIRE(*r[1].as_int64("id") == 2);

            // std::array 数组绑定
            auto ra = co_await sess.query("SELECT id FROM t WHERE id IN (:ids) ORDER BY id",
                                          db::bind("ids", std::array<int64_t, 2> { 2, 1 }));
            REQUIRE(ra.row_count() == 2);
            REQUIRE(*ra[0].as_int64("id") == 1);
            REQUIRE(*ra[1].as_int64("id") == 2);

            // prepared_statement 列式数组绑定（长度 1 = 一行多列）
            auto stmt = sess.stmt("INSERT INTO t (id, a, b) VALUES (:ids, :as, :bs)");
            co_await stmt.bind("ids", std::vector<int64_t> { 3 })
                .bind("as", std::vector<int64_t> { 1 })
                .bind("bs", std::vector<std::string> { std::string("z") })
                .execute();
            auto rs = co_await sess.query("SELECT a, b FROM t WHERE id = 3");
            REQUIRE(*rs[0].as_int64("a") == 1);
            REQUIRE(*rs[0].as_string("b") == "z");

            // 位置列式数组绑定（prepared_statement）
            auto stmt2 = sess.stmt("INSERT INTO t (id, a, b) VALUES (:c1, :c2, :c3)");
            co_await stmt2.bind(std::vector<int64_t> { 4 })
                .bind(std::vector<int64_t> { 9 })
                .bind(std::vector<std::string> { std::string("w") })
                .execute();
            auto rs2 = co_await sess.query("SELECT a, b FROM t WHERE id = 4");
            REQUIRE(*rs2[0].as_int64("a") == 9);
            REQUIRE(*rs2[0].as_string("b") == "w");

            // 空数组 → 异常
            REQUIRE_THROWS_AS(
                co_await sess.query("SELECT id FROM t WHERE id IN (:ids)", db::bind("ids", std::vector<int64_t> {})),
                std::runtime_error);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): batch insert 1000 rows", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");
            co_await sess.query("CREATE TABLE t (a INTEGER, b INTEGER, c INTEGER)");

            std::vector<int64_t> a, b, c;
            a.reserve(1000);
            b.reserve(1000);
            c.reserve(1000);
            for (int64_t i = 0; i < 1000; ++i)
            {
                a.push_back(i);
                b.push_back(i * 2);
                c.push_back(i * 3);
            }

            auto ins = co_await sess.query("INSERT INTO t (a, b, c) VALUES (:a, :b, :c)",
                                           db::bind("a", a),
                                           db::bind("b", b),
                                           db::bind("c", c));
            REQUIRE(ins.affected_rows() == 1000);

            auto r = co_await sess.query("SELECT COUNT(*), SUM(a), SUM(b), SUM(c) FROM t");
            REQUIRE(*r[0].as_int64("COUNT(*)") == 1000);
            REQUIRE(*r[0].as_int64("SUM(a)") == 499500);
            REQUIRE(*r[0].as_int64("SUM(b)") == 999000);
            REQUIRE(*r[0].as_int64("SUM(c)") == 1498500);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): repeated named placeholders", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");

            // 同一个命名参数出现多次：每处占位符都取同一绑定值
            auto r = co_await sess.stmt("SELECT :a + :a AS x").bind("a", 21).execute();
            REQUIRE(*r[0].as_int64("x") == 42);

            // 与另一个命名参数交错
            auto r2 = co_await sess.stmt("SELECT :a + :b + :a AS x").bind("a", 10).bind("b", 5).execute();
            REQUIRE(*r2[0].as_int64("x") == 25);

            // query 路径同样支持
            auto r3 = co_await sess.query("SELECT :a + :a AS x", db::bind("a", 7));
            REQUIRE(*r3[0].as_int64("x") == 14);

            // 同名重复绑定 → 后者生效（每处占位符都取最终值）
            auto r4 = co_await sess.stmt("SELECT :a + :a AS x").bind("a", 1).bind("a", 2).execute();
            REQUIRE(*r4[0].as_int64("x") == 4);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): query bind parity with stmt", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");

            // 位置绑定
            auto r = co_await sess.query("SELECT :a + :b AS x", db::bind(1), db::bind(2));
            REQUIRE(*r[0].as_int64("x") == 3);

            // 位置绑定 + into
            std::optional<int64_t> v;
            co_await sess.query("SELECT :a AS x", db::bind(42), db::into(v, 0));
            REQUIRE(*v == 42);

            // 命名绑定
            auto r2 = co_await sess.query("SELECT :a AS x", db::bind("a", 42));
            REQUIRE(*r2[0].as_int64("x") == 42);

            // 同名重复绑定 → 后者生效
            auto r3 = co_await sess.query("SELECT :a AS x", db::bind("a", 1), db::bind("a", 2));
            REQUIRE(*r3[0].as_int64("x") == 2);

            // 位置/命名混用 → 通用层抛
            REQUIRE_THROWS_AS(co_await sess.query("SELECT :a AS x", db::bind(1), db::bind("a", 2)), std::runtime_error);

            // 位置绑定数量多于占位符 → sqlite 层抛 db_exception
            REQUIRE_THROWS_AS(co_await sess.query("SELECT :a AS x", db::bind(1), db::bind(2)), db::db_exception);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db(sqlite): execute with bind args", "[db][sqlite]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), "sqlite", "db=:memory:");

            // 命名 + into
            std::optional<int64_t> v;
            co_await sess.stmt("SELECT :a AS x").execute(db::bind("a", 42), db::into(v, 0));
            REQUIRE(*v == 42);

            // 位置绑定
            auto r = co_await sess.stmt("SELECT :a + :b AS x").execute(db::bind(10), db::bind(32));
            REQUIRE(*r[0].as_int64("x") == 42);

            // 数组展开（IN）
            co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY)");
            co_await sess.query("INSERT INTO t VALUES (1), (2), (3)");
            auto rs = co_await sess.stmt("SELECT COUNT(*) AS c FROM t WHERE id IN (:ids)")
                          .execute(db::bind("ids", std::vector<int64_t> { 1, 3 }));
            REQUIRE(*rs[0].as_int64("c") == 2);

            // 数组展开（VALUES 多行插入）
            auto ins = co_await sess.stmt("INSERT INTO t (id) VALUES (:row)")
                           .execute(db::bind("row", std::vector<db::param> { db::to_param(4), db::to_param(5) }));
            REQUIRE(ins.affected_rows() == 2);
            auto rw = co_await sess.query("SELECT id FROM t WHERE id IN (4, 5)");
            REQUIRE(rw.row_count() == 2);

            // 与链式 bind 混用
            auto mix = co_await sess.stmt("SELECT :a + :b AS x").bind(10).execute(db::bind(32));
            REQUIRE(*mix[0].as_int64("x") == 42);

            // 绑定了 SQL 中不存在的名字 → 抛
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").execute(db::bind("nope", 1)), std::runtime_error);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: render_query placeholder comes from backend", "[db]")
{
    namespace detail = db::detail;

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

    // 后端决定占位符文本：默认 `?`（MySQL/SQLite 同款）
    stub_backend def;
    auto r1 = detail::render_query("SELECT :a + :a AS x", { db::bind("a", 5) }, def);
    REQUIRE(r1.sql == "SELECT ? + ? AS x");
    REQUIRE(r1.params.size() == 2);

    // `$N` 风格：同名重复也独立编号（每处独立参数）
    struct dollar_backend : stub_backend
    {
        std::string
        placeholder(size_t index, std::string_view) const override
        {
            return "$" + std::to_string(index + 1);
        }
    };
    dollar_backend dollar;
    auto r2 = detail::render_query("SELECT :a + :a AS x", { db::bind("a", 5) }, dollar);
    REQUIRE(r2.sql == "SELECT $1 + $2 AS x");
    REQUIRE(r2.params.size() == 2);

    // `$N` 数组展开 + 标量混合编号
    auto r3 = detail::render_query("SELECT * FROM t WHERE id = :id AND x IN (:xs)",
                                   { db::bind("id", 7), db::bind("xs", std::vector<int64_t> { 1, 2 }) },
                                   dollar);
    REQUIRE(r3.sql == "SELECT * FROM t WHERE id = $1 AND x IN ($2,$3)");
    REQUIRE(r3.params.size() == 3);
    REQUIRE(r3.expanded);

    // `$N` 位置绑定按序编号
    auto r4 = detail::render_query("SELECT :a + :b AS x", { db::bind(1), db::bind(2) }, dollar);
    REQUIRE(r4.sql == "SELECT $1 + $2 AS x");
    REQUIRE(r4.params.size() == 2);

    // 命名风格后端：直接使用参数名
    struct name_backend : stub_backend
    {
        std::string
        placeholder(size_t, std::string_view name) const override
        {
            return name.empty() ? "?" : ":" + std::string(name);
        }
    };
    name_backend named;
    auto r5 = detail::render_query("SELECT :a + :a AS x", { db::bind("a", 5) }, named);
    REQUIRE(r5.sql == "SELECT :a + :a AS x");
    REQUIRE(r5.params.size() == 2);

    // VALUES 数组 → 多行展开（批量插入）
    auto r6 = detail::render_query("INSERT INTO t (id) VALUES (:row)",
                                   { db::bind("row", std::vector<db::param> { db::to_param(4), db::to_param(5) }) },
                                   def);
    REQUIRE(r6.sql == "INSERT INTO t (id) VALUES (?),(?)");
    REQUIRE(r6.params.size() == 2);
    REQUIRE(r6.expanded);

    // VALUES 多行 + `$N` 风格
    auto r7 = detail::render_query("INSERT INTO t (id) VALUES (:row)",
                                   { db::bind("row", std::vector<int64_t> { 1, 2, 3 }) },
                                   dollar);
    REQUIRE(r7.sql == "INSERT INTO t (id) VALUES ($1),($2),($3)");
    REQUIRE(r7.params.size() == 3);

    // VALUES 多列多行：每个占位符是一列的所有数据，等长数组展开为多行
    auto r8 = detail::render_query("INSERT INTO t (id, a, b) VALUES (:ids, :as, :bs)",
                                   { db::bind("ids", std::vector<int64_t> { 1, 2 }),
                                     db::bind("as", std::vector<int64_t> { 42, 7 }),
                                     db::bind("bs", std::vector<std::string> { std::string("x"), std::string("y") }) },
                                   def);
    REQUIRE(r8.sql == "INSERT INTO t (id, a, b) VALUES (?,?,?),(?,?,?)");
    REQUIRE(r8.params.size() == 6);
    REQUIRE(r8.expanded);

    // 关键字小写：`values` 同样触发按列展开
    auto r9 = detail::render_query("insert into t (id) values (:row)",
                                   { db::bind("row", std::vector<int64_t> { 1, 2 }) },
                                   def);
    REQUIRE(r9.sql == "insert into t (id) values (?),(?)");
    REQUIRE(r9.params.size() == 2);
    REQUIRE(r9.expanded);

    // 关键字大小写混合：`ValueS` 同样触发
    auto r10 = detail::render_query("INSERT INTO t (id) ValueS (:row)",
                                    { db::bind("row", std::vector<int64_t> { 1, 2 }) },
                                    def);
    REQUIRE(r10.sql == "INSERT INTO t (id) ValueS (?),(?)");
    REQUIRE(r10.params.size() == 2);
    REQUIRE(r10.expanded);

    // 字符串字面量 "values"（普通位置）不会误触发 VALUES 分支
    auto r11 = detail::render_query("SELECT :a AS x FROM t WHERE s = 'values'", { db::bind("a", 7) }, def);
    REQUIRE(r11.sql == "SELECT ? AS x FROM t WHERE s = 'values'");
    REQUIRE(r11.params.size() == 1);
    REQUIRE(!r11.expanded);

    // 绑定值就是字符串 "values"
    auto r12 = detail::render_query("INSERT INTO t (s) VALUES (:s)", { db::bind("s", std::string("values")) }, def);
    REQUIRE(r12.sql == "INSERT INTO t (s) VALUES (?)");
    REQUIRE(r12.params.size() == 1);
    REQUIRE(!r12.expanded);

    // VALUES 组内字符串字面量 + 占位符（单行）：字面量保留，占位符原地展开
    auto r13 = detail::render_query("INSERT INTO t (s, n) VALUES ('values', :n)", { db::bind("n", 3) }, def);
    REQUIRE(r13.sql == "INSERT INTO t (s, n) VALUES ('values', ?)");
    REQUIRE(r13.params.size() == 1);
    REQUIRE(!r13.expanded);
}
#endif
