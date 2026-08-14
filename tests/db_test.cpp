#ifdef HTTPLIB_ENABLED_DATABASE
#include "common.hpp"
#include "httplib/mysql/config.hpp"
#include "httplib/mysql/connection_pool.hpp"
#include "httplib/mysql/mysql_exception.hpp"
#include "httplib/mysql/result.hpp"
#include "httplib/mysql/session.hpp"
#include "httplib/server/middleware/data.hpp"
#include "httplib/server/middleware/mysql_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>

namespace mysql = httplib::mysql;
namespace mw = httplib::server::middleware;
namespace net = httplib::net;

namespace
{

    mysql::pool_params
    make_cfg()
    {
        mysql::pool_params cfg;
        cfg.user = "root";
        cfg.password = "123456";
        cfg.min_connections = 1;
        cfg.max_connections = 4;
        return cfg;
    }

    template <typename F>
    void
    run(F&& f)
    {
        net::io_context ioc;
        std::exception_ptr err;
        net::co_spawn(
            ioc,
            [&]() -> net::awaitable<void>
            {
                mysql::connection_pool pool(ioc.get_executor(), make_cfg());
                pool.start();
                auto handle = co_await pool.async_acquire();
                co_await handle->query("CREATE DATABASE IF NOT EXISTS test");
                co_await handle->query("USE test");
                co_await f(*handle);
            },
            [&](std::exception_ptr e) { err = e; });
        ioc.run();
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

    template <typename F>
    void
    run_pool(F&& f)
    {
        net::io_context ioc;
        std::exception_ptr err;
        net::co_spawn(
            ioc,
            [&]() -> net::awaitable<void>
            {
                mysql::connection_pool pool(ioc.get_executor(), make_cfg());
                pool.start();
                co_await f(pool);
            },
            [&](std::exception_ptr e) { err = e; });
        ioc.run();
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

} // namespace

// ===========================================================================
// Unit tests — no MySQL required
// ===========================================================================

TEST_CASE("mysql_exception: stores error_code and what", "[db]")
{
    auto ec = boost::system::errc::make_error_code(boost::system::errc::permission_denied);
    mysql::mysql_exception ex(ec, "bad sql");
    REQUIRE(ex.code().value() == ec.value());
    REQUIRE(std::string(ex.what()) == "bad sql");
}

TEST_CASE("result: empty default-constructed", "[db]")
{
    mysql::result r;
    REQUIRE(r.empty());
    REQUIRE(r.row_count() == 0);
    REQUIRE(r.resultset_count() == 0);
    REQUIRE(r.affected_rows() == 0);
    REQUIRE_FALSE(r.next_resultset());
}

TEST_CASE("config: defaults", "[db]")
{
    mysql::connect_params c;
    REQUIRE(c.host == "127.0.0.1");
    REQUIRE(c.port == 3306);
    REQUIRE(c.connect_timeout == std::chrono::seconds(5));
    REQUIRE(c.max_cached_statements == 64);
    REQUIRE(c.time_zone == "+00:00");

    mysql::pool_params p;
    REQUIRE(p.min_connections == 2);
    REQUIRE(p.max_connections == 16);
    REQUIRE(p.acquire_timeout == std::chrono::seconds(5));
    REQUIRE_FALSE(p.validate_on_borrow);
}

// ===========================================================================
// Connection pool
// ===========================================================================

TEST_CASE("db: connection_pool basics", "[db][integration]")
{
    run_pool(
        [](mysql::connection_pool& pool) -> net::awaitable<void>
        {
            auto handle = co_await pool.async_acquire();
            auto ok = co_await handle->ping();
            REQUIRE(ok);
        });
}

TEST_CASE("db: connection_pool concurrent acquire", "[db][integration]")
{
    auto cfg = make_cfg();
    cfg.min_connections = 2;

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connection_pool pool(ioc.get_executor(), cfg);
            pool.start();
            auto s1 = co_await pool.async_acquire();
            auto s2 = co_await pool.async_acquire();
            auto r1 = co_await s1->query("SELECT 'one' AS v");
            auto r2 = co_await s2->query("SELECT 'two' AS v");
            REQUIRE(*r1[0].as_string("v") == "one");
            REQUIRE(*r2[0].as_string("v") == "two");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

// ===========================================================================
// Basic query + result
// ===========================================================================

TEST_CASE("db: simple query", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            auto r = co_await sess.query("SELECT 42 AS n, 'hi' AS s");
            REQUIRE(r.row_count() == 1);
            REQUIRE(r.column_count() == 2);
            REQUIRE(r.column_name(0) == "n");
            REQUIRE(*r[0].as_int64("n") == 42);
            REQUIRE(*r[0].as_string("s") == "hi");
        });
}

TEST_CASE("db: empty result", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_empty (id INT)");
            co_await sess.query("DELETE FROM __httplib_empty");
            auto r = co_await sess.query("SELECT * FROM __httplib_empty");
            REQUIRE(r.row_count() == 0);
            REQUIRE(r.empty());
            co_await sess.query("DROP TABLE IF EXISTS __httplib_empty");
        });
}

TEST_CASE("db: affected_rows", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_aff (id INT AUTO_INCREMENT PRIMARY KEY, v INT)");
            co_await sess.query("TRUNCATE __httplib_aff");
            auto r1 = co_await sess.query("INSERT INTO __httplib_aff (v) VALUES (100)");
            REQUIRE(r1.affected_rows() == 1);
            auto r2 = co_await sess.query("INSERT INTO __httplib_aff (v) VALUES (200),(300)");
            REQUIRE(r2.affected_rows() == 2);
            co_await sess.query("DROP TABLE IF EXISTS __httplib_aff");
        });
}

TEST_CASE("db: DML metadata (affected_rows / last_insert_id)", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_dml (id INT AUTO_INCREMENT PRIMARY KEY, v INT)");
            co_await sess.query("TRUNCATE __httplib_dml");

            auto ins = co_await sess.query("INSERT INTO __httplib_dml (v) VALUES (1),(2),(3)");
            REQUIRE(ins.affected_rows() == 3);
            REQUIRE(ins.last_insert_id() != 0);

            auto upd = co_await sess.query("UPDATE __httplib_dml SET v = v + 10 WHERE v <= 2");
            REQUIRE(upd.affected_rows() == 2);

            auto del = co_await sess.query("DELETE FROM __httplib_dml WHERE v = 11");
            REQUIRE(del.affected_rows() == 1);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_dml");
        });
}

TEST_CASE("db: result iterator", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_iter (v INT)");
            co_await sess.query("DELETE FROM __httplib_iter");
            co_await sess.query("INSERT INTO __httplib_iter VALUES (1),(2),(3)");
            auto r = co_await sess.query("SELECT v FROM __httplib_iter ORDER BY v");
            std::vector<int64_t> vals;
            for (auto row : r)
            {
                vals.push_back(*row.as_int64("v"));
            }
            REQUIRE(vals == std::vector<int64_t> { 1, 2, 3 });
            co_await sess.query("DROP TABLE IF EXISTS __httplib_iter");
        });
}

// ===========================================================================
// Multi-statement / multi-resultset
// ===========================================================================

TEST_CASE("db: multi-statement query", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_batch (id INT, val VARCHAR(50))");
            co_await sess.query("DELETE FROM __httplib_batch");

            auto r = co_await sess.query("INSERT INTO __httplib_batch VALUES (1, 'a');"
                                         "INSERT INTO __httplib_batch VALUES (2, 'b');"
                                         "SELECT * FROM __httplib_batch ORDER BY id");

            REQUIRE(r.resultset_count() == 3);
            REQUIRE(r.affected_rows() == 1);

            REQUIRE(r.next_resultset());
            REQUIRE(r.affected_rows() == 1);

            REQUIRE(r.next_resultset());
            REQUIRE(r.row_count() == 2);
            REQUIRE(r.column_count() == 2);
            REQUIRE(*r[0].as_int64("id") == 1);
            REQUIRE(*r[1].as_string("val") == "b");

            REQUIRE_FALSE(r.next_resultset());

            co_await sess.query("DROP TABLE IF EXISTS __httplib_batch");
        });
}

// ===========================================================================
// All types + row access
// ===========================================================================

TEST_CASE("db: all types roundtrip", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("DROP TABLE IF EXISTS __httplib_types");
            co_await sess.query("CREATE TABLE __httplib_types ("
                                "  i64 BIGINT, u64 BIGINT UNSIGNED, f64 DOUBLE, str VARCHAR(100),"
                                "  bin BLOB, dt_date DATE, dt_dt DATETIME,"
                                "  dt_ts TIMESTAMP NULL, dt_t TIME)");
            co_await sess.query("INSERT INTO __httplib_types VALUES "
                                "(42, 99, 3.14, 'hello', 'world', "
                                "'2024-01-15', '2024-06-20 18:30:45', "
                                "'2024-06-20 18:30:45', '12:34:56')");
            co_await sess.query("INSERT INTO __httplib_types VALUES "
                                "(NULL, NULL, NULL, NULL, NULL, "
                                "NULL, NULL, NULL, NULL)");

            auto r = co_await sess.query("SELECT * FROM __httplib_types ORDER BY i64 IS NULL, i64");
            REQUIRE(r.row_count() == 2);
            REQUIRE(r.column_count() == 9);

            auto row0 = r[0];
            auto row1 = r[1];

            // --- column type ---
            REQUIRE(r.column_type(0) == mysql::column_type::int64);
            REQUIRE(r.column_type(1) == mysql::column_type::uint64);
            REQUIRE(r.column_type(2) == mysql::column_type::double_);
            REQUIRE(r.column_type(3) == mysql::column_type::string);
            REQUIRE(r.column_type(4) == mysql::column_type::blob);
            REQUIRE(r.column_type(5) == mysql::column_type::date);
            REQUIRE(r.column_type(6) == mysql::column_type::datetime);
            REQUIRE(r.column_type(8) == mysql::column_type::time);

            // --- typed access ---
            REQUIRE(*row0.as_int64("i64") == 42);
            REQUIRE(*row0.as_uint64("u64") == 99);
            REQUIRE(*row0.as_double("f64") == 3.14);
            REQUIRE(*row0.as_float("f64") == 3.14f);
            REQUIRE(*row0.as_string("str") == "hello");
            REQUIRE(*row0.as_bool("i64") == true);
            REQUIRE(row0.as_blob("bin")->size() == 5);

            auto d = *row0.as_date("dt_date");
            REQUIRE(d.year == 2024);
            auto dt = *row0.as_datetime("dt_dt");
            REQUIRE(dt.hour == 18);
            auto ts = *row0.as_timestamp("dt_dt");
            REQUIRE(ts > std::chrono::system_clock::from_time_t(0));
            auto t = *row0.as_time("dt_t");
            REQUIRE(t.hour == 12);

            // --- row::get<T> ---
            REQUIRE(*row0.get<int64_t>("i64") == 42);
            REQUIRE(*row0.get<std::string>("str") == "hello");
            REQUIRE(row0.get<mysql::date>("dt_date")->year == 2024);

            // --- NULL handling ---
            REQUIRE(row1.is_null("i64"));
            REQUIRE(!row1.as_int64("i64").has_value());
            REQUIRE(!row1.as_string("str").has_value());
            REQUIRE(row1.as_int64("i64").value_or(-1) == -1);
            REQUIRE(row1.as_string("str").value_or("n/a") == "n/a");
            REQUIRE(row1.get<int64_t>("i64").value_or(-1) == -1);

            // --- type mismatch throws ---
            REQUIRE_THROWS_AS(row0.as_int64("str"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_date("i64"), std::runtime_error);

            // --- column not found ---
            REQUIRE_THROWS_AS(r.column_index("no_such_col"), std::runtime_error);

            // --- iterator ---
            std::vector<std::string> strs;
            for (auto row : r)
            {
                strs.push_back(row.is_null("str") ? "(null)" : std::string(*row.as_string("str")));
            }
            REQUIRE(strs == std::vector<std::string> { "hello", "(null)" });

            co_await sess.query("DROP TABLE IF EXISTS __httplib_types");
        });
}

// ===========================================================================
// Prepared statements
// ===========================================================================

TEST_CASE("db: prepared statement positional bind", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ps (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_ps");
            co_await sess.query("INSERT INTO __httplib_ps VALUES (1, 'alice')");

            auto r = co_await sess.stmt("SELECT id, name FROM __httplib_ps WHERE id = :id").bind(1).execute();
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_int64("id") == 1);
            REQUIRE(*r[0].as_string("name") == "alice");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ps");
        });
}

TEST_CASE("db: prepared statement named params", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_named (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_named");
            co_await sess.query("INSERT INTO __httplib_named VALUES (77, 'target')");

            auto r = co_await sess.stmt("SELECT id, name FROM __httplib_named WHERE name = :n")
                         .bind("n", "target")
                         .execute();
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_int64("id") == 77);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_named");
        });
}

TEST_CASE("db: prepared statement into() extraction", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_into (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_into");
            co_await sess.query("INSERT INTO __httplib_into VALUES (42, 'alice')");

            std::optional<int64_t> id;
            std::optional<std::string> name;
            co_await sess.stmt("SELECT id, name FROM __httplib_into").into(id, "id").into(name, "name").execute();
            REQUIRE(id == 42);
            REQUIRE(name == "alice");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_into");
        });
}

TEST_CASE("db: into() supports small integer types", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ints (a INT, b INT, c INT, d INT)");
            co_await sess.query("DELETE FROM __httplib_ints");
            co_await sess.query("INSERT INTO __httplib_ints VALUES (1, 2, 3, 4)");

            std::optional<int> si;
            std::optional<short> ss;
            std::optional<unsigned> ui;
            std::optional<unsigned short> us;
            co_await sess.stmt("SELECT a, b, c, d FROM __httplib_ints")
                .into(si, 0)
                .into(ss, 1)
                .into(ui, 2)
                .into(us, 3)
                .execute();
            REQUIRE(si == 1);
            REQUIRE(ss == 2);
            REQUIRE(ui == 3u);
            REQUIRE(us == 4);

            std::vector<int> vi;
            std::vector<unsigned> vu;
            co_await sess.query("INSERT INTO __httplib_ints VALUES (5, 6, 7, 8)");
            co_await sess.stmt("SELECT a, c FROM __httplib_ints ORDER BY a").into(vi, 0).into(vu, 1).execute();
            REQUIRE(vi == std::vector<int> { 1, 5 });
            REQUIRE(vu == std::vector<unsigned> { 3u, 7u });

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ints");
        });
}

TEST_CASE("db: statement reuse (caching)", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_cache (id INT)");
            co_await sess.query("DELETE FROM __httplib_cache");
            co_await sess.query("INSERT INTO __httplib_cache VALUES (1),(2)");

            auto stmt = sess.stmt("SELECT id FROM __httplib_cache WHERE id = :id");
            std::optional<int64_t> v1;
            co_await stmt.bind(1).into(v1, 0).execute();
            REQUIRE(v1 == 1);

            std::optional<int64_t> v2;
            co_await stmt.bind(2).into(v2, 0).execute();
            REQUIRE(v2 == 2);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_cache");
        });
}

// ===========================================================================
// Statement cache eviction
// ===========================================================================

TEST_CASE("db: statement cache eviction", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connect_params cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.max_cached_statements = 2;
            auto sess = co_await mysql::session::connect(ioc.get_executor(), cfg);
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_sc (id INT)");
            co_await sess.query("DELETE FROM __httplib_sc");
            co_await sess.query("INSERT INTO __httplib_sc VALUES (1),(2),(3),(4),(5),(6)");

            for (int round = 0; round < 3; ++round)
            {
                std::optional<int64_t> a, b, c;
                co_await sess.stmt("SELECT id FROM __httplib_sc WHERE id = :id").bind(1).into(a, 0).execute();
                co_await sess.stmt("SELECT id FROM __httplib_sc WHERE id > :id").bind(3).into(b, 0).execute();
                co_await sess.stmt("SELECT id FROM __httplib_sc WHERE id >= :id").bind(5).into(c, 0).execute();
                REQUIRE(a == 1);
                REQUIRE(b == 4);
                REQUIRE(c == 5);
            }

            co_await sess.query("DROP TABLE IF EXISTS __httplib_sc");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

// ===========================================================================
// Risk-item regressions
// ===========================================================================

TEST_CASE("db: named params bound out of SQL order", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ord (a INT, b INT)");
            co_await sess.query("DELETE FROM __httplib_ord");
            co_await sess.query("INSERT INTO __httplib_ord VALUES (1, 2)");

            // bind in reverse order of appearance in the SQL
            auto r = co_await sess.stmt("SELECT a, b FROM __httplib_ord WHERE a = :a AND b = :b")
                         .bind("b", 2)
                         .bind("a", 1)
                         .execute();
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_int64("a") == 1);
            REQUIRE(*r[0].as_int64("b") == 2);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ord");
        });
}

TEST_CASE("db: named params reuse across cache", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_nc (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_nc");
            co_await sess.query("INSERT INTO __httplib_nc VALUES (1, 'a'),(2, 'b')");

            for (int round = 0; round < 2; ++round)
            {
                std::optional<int64_t> id;
                std::optional<std::string> name;
                co_await sess.stmt("SELECT id, name FROM __httplib_nc WHERE name = :n")
                    .bind("n", "a")
                    .into(id, "id")
                    .into(name, "name")
                    .execute();
                REQUIRE(id == 1);
                REQUIRE(name == "a");
            }

            co_await sess.query("DROP TABLE IF EXISTS __httplib_nc");
        });
}

// Regression: releasing a session with an open transaction used to crash
// ~session() (use-after-move of conn). It must roll back and leave the pool usable.
TEST_CASE("db: release with open transaction does not crash", "[db][integration]")
{
    run_pool(
        [](mysql::connection_pool& pool) -> net::awaitable<void>
        {
            {
                auto handle = co_await pool.async_acquire();
                co_await handle->begin_transaction();
                co_await handle->query("SELECT 1");
            } // released with the transaction still open

            auto h2 = co_await pool.async_acquire();
            auto ok = co_await h2->ping();
            REQUIRE(ok);
        });
}

TEST_CASE("db: pooled session reconnect keeps params", "[db][integration]")
{
    run_pool(
        [](mysql::connection_pool& pool) -> net::awaitable<void>
        {
            auto handle = co_await pool.async_acquire();
            co_await handle->reconnect();
            auto r = co_await handle->query("SELECT 1 AS v");
            REQUIRE(*r[0].as_int64("v") == 1);
        });
}

TEST_CASE("db: charset applied on connect", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connect_params cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.charset = "latin1";
            auto sess = co_await mysql::session::connect(ioc.get_executor(), cfg);
            auto r = co_await sess.query("SELECT @@character_set_client AS cs");
            REQUIRE(*r[0].as_string("cs") == "latin1");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: async_acquire honors acquire_timeout", "[db][integration]")
{
    auto cfg = make_cfg();
    cfg.min_connections = 0;
    cfg.max_connections = 1;
    cfg.acquire_timeout = std::chrono::seconds(1);

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connection_pool pool(ioc.get_executor(), cfg);
            pool.start();
            auto h1 = co_await pool.async_acquire(); // holds the only connection

            auto t0 = std::chrono::steady_clock::now();
            REQUIRE_THROWS_AS(co_await pool.async_acquire(), std::runtime_error);
            auto elapsed = std::chrono::steady_clock::now() - t0;
            REQUIRE(elapsed >= std::chrono::milliseconds(800));
            REQUIRE(elapsed < std::chrono::seconds(3));
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: transport error marks connection dead", "[db][integration]")
{
    auto cfg = make_cfg();
    cfg.min_connections = 0;
    cfg.max_connections = 1;

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connection_pool pool(ioc.get_executor(), cfg);
            pool.start();

            {
                auto h1 = co_await pool.async_acquire();
                auto r = co_await h1->query("SELECT CONNECTION_ID() AS id");
                auto conn_id = *r[0].as_uint64("id");

                mysql::connect_params kcfg;
                kcfg.user = "root";
                kcfg.password = "123456";
                auto killer = co_await mysql::session::connect(ioc.get_executor(), kcfg);
                co_await killer.query("KILL " + std::to_string(conn_id));

                REQUIRE_THROWS_AS(co_await h1->query("SELECT 1"), mysql::mysql_exception);
            } // h1 released here with a dead connection (live=false)

            // the pool must drop the dead connection and create a fresh one
            auto h2 = co_await pool.async_acquire();
            auto ok = co_await h2->ping();
            REQUIRE(ok);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: borrow ping drops stale connection", "[db][integration]")
{
    auto cfg = make_cfg();
    cfg.min_connections = 0;
    cfg.max_connections = 1;
    cfg.validate_on_borrow = true;

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connection_pool pool(ioc.get_executor(), cfg);
            pool.start();

            {
                auto h1 = co_await pool.async_acquire();
                auto r = co_await h1->query("SELECT CONNECTION_ID() AS id");
                auto conn_id = *r[0].as_uint64("id");

                // kill h1 的连接，但不再执行查询（live 仍为 true）
                mysql::connect_params kcfg;
                kcfg.user = "root";
                kcfg.password = "123456";
                auto killer = co_await mysql::session::connect(ioc.get_executor(), kcfg);
                co_await killer.query("KILL " + std::to_string(conn_id));
            } // h1 释放回池，live 仍为 true，但底层连接已死

            // 借出时 ping 应检测到失效连接并丢弃、重建
            auto h2 = co_await pool.async_acquire();
            auto ok = co_await h2->ping();
            REQUIRE(ok);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

// Regression: binding two owned-storage values (json) used to share a single
// data_str buffer, so the first field_view dangled / was overwritten.
TEST_CASE("db: prepared statement multiple json binds", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_j2 (a JSON, b JSON)");
            co_await sess.query("DELETE FROM __httplib_j2");

            boost::json::value v1 = {
                { "x", 1 }
            };
            boost::json::value v2 = {
                { "y", "hi" }
            };
            co_await sess.stmt("INSERT INTO __httplib_j2 (a, b) VALUES (:a, :b)").bind(v1).bind(v2).execute();

            auto r = co_await sess.query("SELECT a, b FROM __httplib_j2");
            REQUIRE(r.row_count() == 1);
            REQUIRE(r[0].as_json("a")->as_object().at("x").as_int64() == 1);
            REQUIRE(r[0].as_json("b")->as_object().at("y").as_string() == "hi");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_j2");
        });
}

TEST_CASE("db: numeric conversion range checks", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_rng (u BIGINT UNSIGNED, i BIGINT)");
            co_await sess.query("DELETE FROM __httplib_rng");
            co_await sess.query("INSERT INTO __httplib_rng VALUES (18446744073709551615, -1)");
            auto r = co_await sess.query("SELECT u, i FROM __httplib_rng");

            REQUIRE_THROWS_AS(r[0].as_int64("u"), std::runtime_error);
            REQUIRE_THROWS_AS(r[0].as_uint64("i"), std::runtime_error);
            REQUIRE(*r[0].as_uint64("u") == 18446744073709551615ull);
            REQUIRE(*r[0].as_int64("i") == -1);
            REQUIRE(*r[0].as_bool("u") == true);

            REQUIRE_THROWS_AS(r.column_name(999), std::out_of_range);
            REQUIRE_THROWS_AS(r.column_type(999), std::out_of_range);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_rng");
        });
}

TEST_CASE("db: double to int range check", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            auto r1 = co_await sess.query("SELECT 1e20 AS v");
            REQUIRE_THROWS_AS(r1[0].as_int64("v"), std::runtime_error);
            REQUIRE_THROWS_AS(r1[0].as_uint64("v"), std::runtime_error);

            auto r2 = co_await sess.query("SELECT -1.5e0 AS v");
            REQUIRE_THROWS_AS(r2[0].as_uint64("v"), std::runtime_error);

            auto r3 = co_await sess.query("SELECT 4.2e1 AS v");
            REQUIRE(*r3[0].as_int64("v") == 42);
        });
}

TEST_CASE("db: negative TIME parsed", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_nt (t TIME)");
            co_await sess.query("DELETE FROM __httplib_nt");
            co_await sess.query("INSERT INTO __httplib_nt VALUES ('-12:34:56')");
            auto r = co_await sess.query("SELECT t FROM __httplib_nt");
            auto t = *r[0].as_time("t");
            REQUIRE(t.negative);
            REQUIRE(t.hour == 12);
            REQUIRE(t.minute == 34);
            REQUIRE(t.second == 56);
            co_await sess.query("DROP TABLE IF EXISTS __httplib_nt");
        });
}

TEST_CASE("db: timestamp timezone conversion", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connect_params cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.time_zone = "+08:00";
            auto sess = co_await mysql::session::connect(ioc.get_executor(), cfg);
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ts (t TIMESTAMP)");
            co_await sess.query("DELETE FROM __httplib_ts");
            co_await sess.query("INSERT INTO __httplib_ts VALUES ('2024-01-01 12:00:00')");
            auto r = co_await sess.query("SELECT t FROM __httplib_ts");

            REQUIRE(r.column_type(0) == mysql::column_type::timestamp);
            auto ts = *r[0].as_timestamp("t");
            std::chrono::system_clock::time_point expected
                = std::chrono::sys_days(std::chrono::year(2024) / std::chrono::month(1) / std::chrono::day(1))
                  + std::chrono::hours(4);
            REQUIRE(ts == expected);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ts");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: timestamp default utc", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connect_params cfg;
            cfg.user = "root";
            cfg.password = "123456"; // 默认 time_zone = "+00:00"
            auto sess = co_await mysql::session::connect(ioc.get_executor(), cfg);
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_tsu (t TIMESTAMP)");
            co_await sess.query("DELETE FROM __httplib_tsu");
            co_await sess.query("INSERT INTO __httplib_tsu VALUES ('2024-01-01 12:00:00')");
            auto r = co_await sess.query("SELECT t FROM __httplib_tsu");

            auto ts = *r[0].as_timestamp("t");
            std::chrono::system_clock::time_point expected
                = std::chrono::sys_days(std::chrono::year(2024) / std::chrono::month(1) / std::chrono::day(1))
                  + std::chrono::hours(12);
            REQUIRE(ts == expected);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_tsu");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: timestamp bind roundtrip utc", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_tsr (t TIMESTAMP)");
            co_await sess.query("DELETE FROM __httplib_tsr");

            std::chrono::system_clock::time_point tp
                = std::chrono::sys_days(std::chrono::year(2024) / std::chrono::month(1) / std::chrono::day(1))
                  + std::chrono::hours(12);
            co_await sess.stmt("INSERT INTO __httplib_tsr (t) VALUES (:t)").bind(tp).execute();
            auto r = co_await sess.query("SELECT t FROM __httplib_tsr");
            REQUIRE(*r[0].as_timestamp("t") == tp);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_tsr");
        });
}

TEST_CASE("db: timestamp bind roundtrip offset", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connect_params cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.time_zone = "+08:00";
            auto sess = co_await mysql::session::connect(ioc.get_executor(), cfg);
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_tso (t TIMESTAMP)");
            co_await sess.query("DELETE FROM __httplib_tso");

            std::chrono::system_clock::time_point tp
                = std::chrono::sys_days(std::chrono::year(2024) / std::chrono::month(1) / std::chrono::day(1))
                  + std::chrono::hours(12);
            co_await sess.stmt("INSERT INTO __httplib_tso (t) VALUES (:t)").bind(tp).execute();
            auto r = co_await sess.query("SELECT t FROM __httplib_tso");
            REQUIRE(*r[0].as_timestamp("t") == tp);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_tso");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: datetime date time unaffected by timezone", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connect_params cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.time_zone = "+08:00";
            auto sess = co_await mysql::session::connect(ioc.get_executor(), cfg);
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_tzn (d DATETIME, dt DATE, t TIME)");
            co_await sess.query("DELETE FROM __httplib_tzn");
            co_await sess.query("INSERT INTO __httplib_tzn VALUES ('2024-01-15 12:34:56', '2024-01-15', '12:34:56')");
            auto r = co_await sess.query("SELECT d, dt, t FROM __httplib_tzn");

            REQUIRE(r.column_type(0) == mysql::column_type::datetime);
            auto d = *r[0].as_datetime("d");
            REQUIRE(d.hour == 12);
            REQUIRE(d.minute == 34);

            auto dt = *r[0].as_date("dt");
            REQUIRE(dt.year == 2024);
            REQUIRE(dt.day == 15);

            auto t = *r[0].as_time("t");
            REQUIRE(t.hour == 12);
            REQUIRE(t.second == 56);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_tzn");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: into replaced across executes", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ext (id INT)");
            co_await sess.query("DELETE FROM __httplib_ext");
            co_await sess.query("INSERT INTO __httplib_ext VALUES (1),(2)");

            auto stmt = sess.stmt("SELECT id FROM __httplib_ext WHERE id = :id");
            std::optional<int64_t> v1;
            co_await stmt.bind(1).into(v1, 0).execute();
            REQUIRE(v1 == 1);

            std::optional<int64_t> v2;
            co_await stmt.bind(2).into(v2, 0).execute();
            REQUIRE(v1 == 1); // 重新 into 替换旧 extractor，v1 不再被写
            REQUIRE(v2 == 2);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ext");
        });
}

TEST_CASE("db: into persists across executes", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ip (v INT)");
            co_await sess.query("DELETE FROM __httplib_ip");
            co_await sess.query("INSERT INTO __httplib_ip VALUES (10),(20)");

            // 输出只绑一次，循环里只重绑输入，输出每次自动更新
            auto stmt = sess.stmt("SELECT v FROM __httplib_ip WHERE v = :v");
            std::optional<int64_t> out;
            stmt.into(out, "v");
            co_await stmt.bind(10).execute();
            REQUIRE(out == 10);
            co_await stmt.bind(20).execute();
            REQUIRE(out == 20);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ip");
        });
}

TEST_CASE("db: into vector", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_vec (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_vec");
            co_await sess.query("INSERT INTO __httplib_vec VALUES (1, 'a'),(2, 'b'),(3, 'c')");

            // 按列下标
            std::vector<int64_t> ids;
            co_await sess.stmt("SELECT id FROM __httplib_vec ORDER BY id").into(ids, 0).execute();
            REQUIRE(ids == std::vector<int64_t> { 1, 2, 3 });

            // 按列名
            std::vector<std::string> names;
            co_await sess.stmt("SELECT name FROM __httplib_vec ORDER BY id").into(names, "name").execute();
            REQUIRE(names == std::vector<std::string> { "a", "b", "c" });

            // 含 NULL 行时抛异常
            co_await sess.query("INSERT INTO __httplib_vec VALUES (NULL, NULL)");
            std::vector<int64_t> all;
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT id FROM __httplib_vec").into(all, "id").execute(),
                              std::runtime_error);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_vec");
        });
}

TEST_CASE("db: into positional column", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ipc (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_ipc");
            co_await sess.query("INSERT INTO __httplib_ipc VALUES (1, 'a'),(2, 'b')");

            // optional：第 N 次 into 对应第 N 列（SOCI 风格，不写列名/列号）
            std::optional<int64_t> id;
            std::optional<std::string> name;
            co_await sess.stmt("SELECT id, name FROM __httplib_ipc WHERE id = :id")
                .bind("id", 1)
                .into(id)
                .into(name)
                .execute();
            REQUIRE(id == 1);
            REQUIRE(name == "a");

            // vector：第 N 次 into 对应第 N 列
            std::vector<int64_t> ids;
            std::vector<std::string> names;
            co_await sess.stmt("SELECT id, name FROM __httplib_ipc ORDER BY id").into(ids).into(names).execute();
            REQUIRE(ids == std::vector<int64_t> { 1, 2 });
            REQUIRE(names == std::vector<std::string> { "a", "b" });

            // 重新 into 时计数器从 0 重新计
            std::optional<int64_t> v;
            auto stmt = sess.stmt("SELECT id FROM __httplib_ipc WHERE id = :id");
            co_await stmt.bind("id", 1).into(v).execute();
            REQUIRE(v == 1);
            co_await stmt.bind("id", 2).into(v).execute();
            REQUIRE(v == 2);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ipc");
        });
}

TEST_CASE("db: bind arity mismatch throws", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_arity (a INT, b INT)");
            co_await sess.query("DELETE FROM __httplib_arity");

            REQUIRE_THROWS_AS(co_await sess.stmt("INSERT INTO __httplib_arity VALUES (:a, :b)").bind(1).execute(),
                              mysql::mysql_exception);
            REQUIRE_THROWS_AS(co_await sess.stmt("INSERT INTO __httplib_arity VALUES (:a)").bind(1).bind(2).execute(),
                              mysql::mysql_exception);

            // 连接仍可用（参数数量错误是客户端判定，不应废掉连接）
            auto r = co_await sess.query("SELECT 1 AS v");
            REQUIRE(*r[0].as_int64("v") == 1);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_arity");
        });
}

TEST_CASE("db: error message includes params", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ep (id INT PRIMARY KEY)");
            co_await sess.query("DELETE FROM __httplib_ep");
            co_await sess.query("INSERT INTO __httplib_ep VALUES (1)");

            try
            {
                co_await sess.stmt("INSERT INTO __httplib_ep VALUES (:id)").bind(1).execute();
                REQUIRE(false);
            }
            catch (mysql::mysql_exception const& ex)
            {
                REQUIRE(std::string(ex.what()).find("params: [1]") != std::string::npos);
            }

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ep");
        });
}

TEST_CASE("db: error message includes named params", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_enp (id INT PRIMARY KEY)");
            co_await sess.query("DELETE FROM __httplib_enp");
            co_await sess.query("INSERT INTO __httplib_enp VALUES (1)");

            try
            {
                co_await sess.stmt("INSERT INTO __httplib_enp VALUES (:id)").bind("id", 1).execute();
                REQUIRE(false);
            }
            catch (mysql::mysql_exception const& ex)
            {
                REQUIRE(std::string(ex.what()).find("params: [id=1]") != std::string::npos);
                REQUIRE(std::string(ex.what()).find(":id") != std::string::npos); // 原始 SQL 保留 :id
            }

            co_await sess.query("DROP TABLE IF EXISTS __httplib_enp");
        });
}

TEST_CASE("db: bind null and empty string", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_bnd (a INT, s VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_bnd");
            co_await sess.stmt("INSERT INTO __httplib_bnd (a, s) VALUES (:a, :s)")
                .bind(nullptr)
                .bind(std::string_view(""))
                .execute();

            auto r = co_await sess.query("SELECT a, s FROM __httplib_bnd");
            REQUIRE(r[0].is_null("a"));
            REQUIRE(*r[0].as_string("s") == "");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_bnd");
        });
}

TEST_CASE("db: bind prevents SQL injection", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_inj (name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_inj");
            co_await sess.query("INSERT INTO __httplib_inj VALUES ('alice'),('bob')");

            auto r
                = co_await sess.stmt("SELECT name FROM __httplib_inj WHERE name = :name").bind("' OR '1'='1").execute();
            REQUIRE(r.row_count() == 0);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_inj");
        });
}

TEST_CASE("db: bind blob binary data", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_blob (b BLOB)");
            co_await sess.query("DELETE FROM __httplib_blob");

            unsigned char raw[] = { 0x00, 0x01, 0x02, 0x00, 0xff, 0x00 };
            co_await sess.stmt("INSERT INTO __httplib_blob (b) VALUES (:b)")
                .bind(std::as_bytes(std::span(raw)))
                .execute();

            auto r = co_await sess.query("SELECT b FROM __httplib_blob");
            auto blob = r[0].as_blob("b");
            REQUIRE(blob.has_value());
            REQUIRE(blob->size() == sizeof(raw));
            REQUIRE(std::string_view(reinterpret_cast<char const*>(blob->data()), blob->size())
                    == std::string_view(reinterpret_cast<char const*>(raw), sizeof(raw)));

            co_await sess.query("DROP TABLE IF EXISTS __httplib_blob");
        });
}

TEST_CASE("db: bind named blob binary data", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_nblob (b BLOB)");
            co_await sess.query("DELETE FROM __httplib_nblob");

            unsigned char raw[] = { 0xde, 0xad, 0xbe, 0xef };
            co_await sess.stmt("INSERT INTO __httplib_nblob (b) VALUES (:b)")
                .bind("b", std::as_bytes(std::span(raw)))
                .execute();

            auto r = co_await sess.query("SELECT b FROM __httplib_nblob");
            auto blob = r[0].as_blob("b");
            REQUIRE(blob.has_value());
            REQUIRE(blob->size() == sizeof(raw));
            REQUIRE(std::string_view(reinterpret_cast<char const*>(blob->data()), blob->size())
                    == std::string_view(reinterpret_cast<char const*>(raw), sizeof(raw)));

            co_await sess.query("DROP TABLE IF EXISTS __httplib_nblob");
        });
}

TEST_CASE("db: unbound named param throws", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            // 完全未绑定
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :n IS NULL AS r").execute(), std::runtime_error);

            // 绑了一部分，剩一个没绑
            REQUIRE_THROWS_AS(
                co_await sess.stmt("SELECT :a + :b AS x").bind("a", 1).execute(), std::runtime_error);

            // 全部绑定正常执行
            auto r = co_await sess.stmt("SELECT :a + :b AS x").bind("a", 1).bind("b", 2).execute();
            REQUIRE(*r[0].as_int64("x") == 3);
        });
}

TEST_CASE("db: named param reused multiple times", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_nr (a INT, b INT)");
            co_await sess.query("DELETE FROM __httplib_nr");
            co_await sess.query("INSERT INTO __httplib_nr VALUES (1, 1),(2, 3)");

            // 同一个 :x 出现两次，绑一次即可两处生效
            auto r = co_await sess.stmt("SELECT a, b FROM __httplib_nr WHERE a = :x AND b = :x").bind("x", 1).execute();
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_int64("a") == 1);
            REQUIRE(*r[0].as_int64("b") == 1);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_nr");
        });
}

TEST_CASE("db: named param cross reuse", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_cross (a INT, b INT, c INT)");
            co_await sess.query("DELETE FROM __httplib_cross");
            co_await sess.query("INSERT INTO __httplib_cross VALUES (1,1,1),(1,2,1),(2,3,2)");

            // 同一个名字出现 3 次，绑一次全部生效
            {
                auto r = co_await sess.stmt("SELECT a, b, c FROM __httplib_cross WHERE a = :x AND b = :x AND c = :x")
                             .bind("x", 1)
                             .execute();
                REQUIRE(r.row_count() == 1);
                REQUIRE(*r[0].as_int64("a") == 1);
                REQUIRE(*r[0].as_int64("b") == 1);
                REQUIRE(*r[0].as_int64("c") == 1);
            }

            // 两个名字交叉 + 重复
            {
                auto r = co_await sess.stmt("SELECT a, b, c FROM __httplib_cross WHERE a = :x AND b = :y AND c = :x")
                             .bind("x", 1)
                             .bind("y", 2)
                             .execute();
                REQUIRE(r.row_count() == 1);
                REQUIRE(*r[0].as_int64("a") == 1);
                REQUIRE(*r[0].as_int64("b") == 2);
                REQUIRE(*r[0].as_int64("c") == 1);
            }

            // 命名绑定与出现顺序无关（先绑 y 再绑 x）
            {
                auto r = co_await sess.stmt("SELECT a, b, c FROM __httplib_cross WHERE a = :x AND b = :y AND c = :x")
                             .bind("y", 2)
                             .bind("x", 1)
                             .execute();
                REQUIRE(r.row_count() == 1);
                REQUIRE(*r[0].as_int64("a") == 1);
                REQUIRE(*r[0].as_int64("b") == 2);
                REQUIRE(*r[0].as_int64("c") == 1);
            }

            // 位置绑定下每个出现都是一个位置（重复名也要逐个绑）
            {
                auto r = co_await sess.stmt("SELECT a, b, c FROM __httplib_cross WHERE a = :x AND b = :y AND c = :x")
                             .bind(1)
                             .bind(2)
                             .bind(1)
                             .execute();
                REQUIRE(r.row_count() == 1);
                REQUIRE(*r[0].as_int64("a") == 1);
                REQUIRE(*r[0].as_int64("b") == 2);
                REQUIRE(*r[0].as_int64("c") == 1);
            }

            co_await sess.query("DROP TABLE IF EXISTS __httplib_cross");
        });
}

TEST_CASE("db: bind unknown name throws", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            // SQL 里只有 :a，绑一个不存在的名字
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind("b", 42).execute(), std::runtime_error);

            // 命名 + 位置混用（两种顺序都抛）
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind(1).bind("a", 2).execute(), std::runtime_error);
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a AS x").bind("a", 2).bind(1).execute(), std::runtime_error);

            // 正常绑定不受影响
            auto r = co_await sess.stmt("SELECT :a AS x").bind("a", 42).execute();
            REQUIRE(*r[0].as_int64("x") == 42);
        });
}

TEST_CASE("db: named placeholder bound positionally", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            // :name 占位符按出现顺序做位置绑定（SOCI 风格）
            auto r = co_await sess.stmt("SELECT :a + :b AS x").bind(1).bind(2).execute();
            REQUIRE(*r[0].as_int64("x") == 3);
        });
}

TEST_CASE("db: bind once execute multiple times", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_reuse (v INT)");
            co_await sess.query("DELETE FROM __httplib_reuse");

            // 命名参数：绑一次跑两次，参数复用（named_values 不消费）
            auto named = sess.stmt("INSERT INTO __httplib_reuse VALUES (:v)");
            named.bind("v", 1);
            co_await named.execute();
            co_await named.execute();

            // 位置参数：绑一次跑两次，参数也应复用
            auto positional = sess.stmt("INSERT INTO __httplib_reuse VALUES (:v)");
            positional.bind(2);
            co_await positional.execute();
            co_await positional.execute();

            auto r1 = co_await sess.query("SELECT COUNT(*) AS c FROM __httplib_reuse WHERE v = 1");
            REQUIRE(*r1[0].as_int64("c") == 2);
            auto r2 = co_await sess.query("SELECT COUNT(*) AS c FROM __httplib_reuse WHERE v = 2");
            REQUIRE(*r2[0].as_int64("c") == 2);

            // 位置式重绑：第二次 bind 应覆盖而非追加
            auto rebind = sess.stmt("INSERT INTO __httplib_reuse VALUES (:v)");
            rebind.bind(3);
            co_await rebind.execute();
            rebind.bind(4);
            co_await rebind.execute();
            auto r3 = co_await sess.query("SELECT COUNT(*) AS c FROM __httplib_reuse WHERE v = 4");
            REQUIRE(*r3[0].as_int64("c") == 1);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_reuse");
        });
}

TEST_CASE("db: into edge cases", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_intoedge (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_intoedge");
            co_await sess.query("INSERT INTO __httplib_intoedge VALUES (1, 'alice'), (NULL, NULL)");

            // 越界列下标
            {
                std::optional<int64_t> v;
                REQUIRE_THROWS_AS(co_await sess.stmt("SELECT id FROM __httplib_intoedge").into(v, 999).execute(),
                                  std::out_of_range);
            }
            // 列名不存在
            {
                std::optional<int64_t> v;
                REQUIRE_THROWS_AS(co_await sess.stmt("SELECT id FROM __httplib_intoedge").into(v, "nope").execute(),
                                  std::runtime_error);
            }
            // 类型不匹配
            {
                std::optional<int64_t> v;
                REQUIRE_THROWS_AS(
                    co_await sess.stmt("SELECT name FROM __httplib_intoedge WHERE id = 1").into(v, "name").execute(),
                    std::runtime_error);
            }
            // 空结果不写 optional
            {
                std::optional<int64_t> v = 7;
                co_await sess.stmt("SELECT id FROM __httplib_intoedge WHERE id = 999").into(v, "id").execute();
                REQUIRE(v == 7);
            }
            // NULL 值 -> nullopt
            {
                std::optional<int64_t> v = 7;
                co_await sess.stmt("SELECT id FROM __httplib_intoedge WHERE name IS NULL").into(v, "id").execute();
                REQUIRE(!v.has_value());
            }

            co_await sess.query("DROP TABLE IF EXISTS __httplib_intoedge");
        });
}

TEST_CASE("db: with_transaction keeps original error", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connection_pool pool(ioc.get_executor(), make_cfg());
            pool.start();
            auto handle = co_await pool.async_acquire();

            mysql::connect_params kcfg;
            kcfg.user = "root";
            kcfg.password = "123456";
            auto killer = co_await mysql::session::connect(ioc.get_executor(), kcfg);

            std::string msg;
            try
            {
                co_await handle->with_transaction(
                    [&](mysql::session& s) -> net::awaitable<void>
                    {
                        auto r = co_await s.query("SELECT CONNECTION_ID() AS id");
                        auto conn_id = *r[0].as_uint64("id");
                        co_await killer.query("KILL " + std::to_string(conn_id));
                        throw std::runtime_error("boom");
                    });
            }
            catch (std::runtime_error const& ex)
            {
                msg = ex.what();
            }
            REQUIRE(msg == "boom");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: dead connection release wakes waiter", "[db][integration]")
{
    auto cfg = make_cfg();
    cfg.min_connections = 0;
    cfg.max_connections = 1;
    cfg.acquire_timeout = std::chrono::seconds(10);

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connection_pool pool(ioc.get_executor(), cfg);
            pool.start();

            auto h1 = co_await pool.async_acquire();
            auto r = co_await h1->query("SELECT CONNECTION_ID() AS id");
            auto conn_id = *r[0].as_uint64("id");

            std::optional<mysql::connection_pool::session_handle> h2;
            net::co_spawn(
                ioc.get_executor(),
                [&pool, &h2]() -> net::awaitable<void> { h2.emplace(co_await pool.async_acquire()); },
                [](std::exception_ptr) {});

            // let the waiting acquire register itself
            net::steady_timer settle(ioc.get_executor());
            settle.expires_after(std::chrono::milliseconds(200));
            co_await settle.async_wait(boost::asio::use_awaitable);

            {
                mysql::connect_params kcfg;
                kcfg.user = "root";
                kcfg.password = "123456";
                auto killer = co_await mysql::session::connect(ioc.get_executor(), kcfg);
                co_await killer.query("KILL " + std::to_string(conn_id));
            }
            REQUIRE_THROWS_AS(co_await h1->query("SELECT 1"), mysql::mysql_exception);

            auto t0 = std::chrono::steady_clock::now();
            h1.release(); // dead -> dropped -> wakes the waiter

            while (!h2)
            {
                net::steady_timer poll(ioc.get_executor());
                poll.expires_after(std::chrono::milliseconds(50));
                co_await poll.async_wait(boost::asio::use_awaitable);
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(3))
                {
                    break;
                }
            }

            REQUIRE(h2.has_value());
            auto ok = co_await (*h2)->ping();
            REQUIRE(ok);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

// ===========================================================================
// JSON column
// ===========================================================================

TEST_CASE("db: json column", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_json (id INT, data JSON)");
            co_await sess.query("DELETE FROM __httplib_json");
            co_await sess.query("INSERT INTO __httplib_json VALUES (1, '{\"x\":1,\"y\":\"hi\"}')");
            auto r = co_await sess.query("SELECT data FROM __httplib_json WHERE id = 1");
            auto val = *r[0].as_json("data");
            REQUIRE(val.as_object().at("x").as_int64() == 1);
            REQUIRE(val.as_object().at("y").as_string() == "hi");
            co_await sess.query("DROP TABLE IF EXISTS __httplib_json");
        });
}

// ===========================================================================
// Transactions
// ===========================================================================

TEST_CASE("db: transaction commit", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_txn (id INT)");
            co_await sess.query("DELETE FROM __httplib_txn");

            co_await sess.begin_transaction();
            co_await sess.query("INSERT INTO __httplib_txn VALUES (100)");
            co_await sess.commit();

            auto r = co_await sess.query("SELECT id FROM __httplib_txn");
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_int64("id") == 100);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_txn");
        });
}

TEST_CASE("db: transaction rollback", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_txr (id INT)");
            co_await sess.query("DELETE FROM __httplib_txr");

            co_await sess.begin_transaction();
            co_await sess.query("INSERT INTO __httplib_txr VALUES (200)");
            co_await sess.rollback();

            auto r = co_await sess.query("SELECT id FROM __httplib_txr");
            REQUIRE(r.row_count() == 0);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_txr");
        });
}

TEST_CASE("db: with_transaction commit path", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_wtxn (id INT)");
            co_await sess.query("DELETE FROM __httplib_wtxn");

            co_await sess.with_transaction(
                [](mysql::session& s) -> net::awaitable<void>
                {
                    co_await s.query("INSERT INTO __httplib_wtxn VALUES (1)");
                    co_await s.query("INSERT INTO __httplib_wtxn VALUES (2)");
                });

            auto r = co_await sess.query("SELECT id FROM __httplib_wtxn ORDER BY id");
            REQUIRE(r.row_count() == 2);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_wtxn");
        });
}

TEST_CASE("db: with_transaction rollback on exception", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_wtxr (id INT)");
            co_await sess.query("DELETE FROM __httplib_wtxr");

            try
            {
                co_await sess.with_transaction(
                    [](mysql::session& s) -> net::awaitable<void>
                    {
                        co_await s.query("INSERT INTO __httplib_wtxr VALUES (999)");
                        throw std::runtime_error("boom");
                    });
            }
            catch (std::runtime_error const&)
            {
            }

            auto r = co_await sess.query("SELECT id FROM __httplib_wtxr");
            REQUIRE(r.row_count() == 0);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_wtxr");
        });
}

// ===========================================================================
// Ping / query_logger / reconnect
// ===========================================================================

TEST_CASE("db: ping", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            auto ok = co_await sess.ping();
            REQUIRE(ok);
        });
}

TEST_CASE("db: query_logger", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            std::string logged;
            size_t rows = 0;
            sess.set_query_logger(
                [&](mysql::query_log_entry const& e)
                {
                    logged = e.sql;
                    rows = e.row_count;
                });
            co_await sess.query("SELECT 42 AS n");
            REQUIRE(logged.find("SELECT 42") != std::string::npos);
            REQUIRE(rows == 1);
        });
}

TEST_CASE("db: standalone connect and query", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connect_params cfg;
            cfg.user = "root";
            cfg.password = "123456";
            auto sess = co_await mysql::session::connect(ioc.get_executor(), cfg);
            auto r = co_await sess.query("SELECT 'standalone' AS msg");
            REQUIRE(*r[0].as_string("msg") == "standalone");
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: standalone reconnect", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connect_params cfg;
            cfg.user = "root";
            cfg.password = "123456";
            auto sess = co_await mysql::session::connect(ioc.get_executor(), cfg);
            auto r1 = co_await sess.query("SELECT 1 AS v");
            REQUIRE(*r1[0].as_int64("v") == 1);
            co_await sess.reconnect();
            auto r2 = co_await sess.query("SELECT 2 AS v");
            REQUIRE(*r2[0].as_int64("v") == 2);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: reconnect resets transaction state", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.begin_transaction();
            REQUIRE(sess.in_transaction());
            co_await sess.reconnect();
            REQUIRE_FALSE(sess.in_transaction());
        });
}

// ===========================================================================
// Error / exception paths
// ===========================================================================

TEST_CASE("db: invalid SQL throws", "[db][integration]")
{
    run([](mysql::session& sess) -> net::awaitable<void>
        { REQUIRE_THROWS_AS(co_await sess.query("BOGUS SYNTAX ERROR"), mysql::mysql_exception); });
}

TEST_CASE("db: row out of bounds", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            auto r = co_await sess.query("SELECT 1 AS v");
            REQUIRE(r.row_count() == 1);
            REQUIRE_NOTHROW(r[0]);
        });
}

TEST_CASE("db: next_resultset on single resultset", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            auto r = co_await sess.query("SELECT 1 AS v");
            REQUIRE(r.resultset_count() == 1);
            REQUIRE_FALSE(r.next_resultset());
            REQUIRE(r.row_count() == 1);
        });
}

TEST_CASE("db: column access on empty resultset", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_nerr (id INT)");
            co_await sess.query("DELETE FROM __httplib_nerr");
            auto r = co_await sess.query("SELECT * FROM __httplib_nerr");
            REQUIRE(r.row_count() == 0);
            REQUIRE(r.column_count() == 1);
            REQUIRE(r.column_name(0) == "id");
            co_await sess.query("DROP TABLE IF EXISTS __httplib_nerr");
        });
}

// ===========================================================================
// Middleware
// ===========================================================================

TEST_CASE("mysql_middleware: throws when not registered", "[db][middleware]")
{
    net::io_context ioc;
    std::exception_ptr err;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            httplib::server::http_server server(ioc.get_executor());
            server.router().template set_http_handler<http::verb::get>(
                "/db/nomw",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    REQUIRE_THROWS_AS(mw::fetch<mw::mysql_middleware>(req), std::exception);
                    resp.set_string_content("ok"sv, "text/plain"sv);
                });
            server.listen("127.0.0.1", 0);
            auto ep = server.local_endpoint();
            server.run();

            httplib::client::http_client client(ioc.get_executor(), ep.address().to_string(), ep.port());
            client.set_timeout(std::chrono::seconds(5));

            auto resp = UNWRAP(co_await client.async_get("/db/nomw"));
            REQUIRE(resp.result() == http::status::ok);

            client.close();
            server.stop();
        },
        [&](std::exception_ptr e) { err = e; });

    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: date/time/datetime utilities", "[db][unit]")
{
    using namespace std::chrono;

    static_assert(mysql::date { 2024, 2, 29 }.is_valid());
    static_assert(!mysql::date { 2023, 2, 29 }.is_valid());
    static_assert(mysql::date { 2024, 2, 29 }.is_leap_year());
    static_assert(mysql::date { 2024, 2, 29 }.days_in_month() == 29);
    static_assert(mysql::date::from_sys_days(mysql::date { 2024, 1, 2 }.to_sys_days()) == mysql::date { 2024, 1, 2 });
    static_assert(mysql::time { 1, 2, 3, 456789 }.to_duration() == microseconds { 3723456789LL });
    static_assert(mysql::time { 1, 2, 3, 456789 }.total_microseconds() == 3723456789LL);
    static_assert(mysql::time::from_duration(microseconds { -1 }) == mysql::time { 0, 0, 0, 1, true });
    static_assert(mysql::datetime { 2024, 1, 15, 12, 34, 56 }.is_valid());

    mysql::date d { 2024, 2, 29 };
    REQUIRE(d.is_valid());
    REQUIRE(d.is_leap_year());
    REQUIRE(d.days_in_month() == 29);
    REQUIRE_FALSE(mysql::date { 2023, 2, 29 }.is_valid());
    REQUIRE(mysql::date { 2023, 2, 28 }.is_valid());
    REQUIRE_FALSE(mysql::date { 2024, 4, 31 }.is_valid());
    REQUIRE_FALSE(mysql::date { 0, 0, 0 }.is_valid());

    REQUIRE(d.to_string() == "2024-02-29");
    auto d2 = mysql::date::from_string("2024-02-29");
    REQUIRE(d2.has_value());
    REQUIRE(*d2 == d);
    REQUIRE_FALSE(mysql::date::from_string("2024-02-30").has_value());
    REQUIRE_FALSE(mysql::date::from_string("2024/02/29").has_value());

    auto sd = d.to_sys_days();
    REQUIRE(mysql::date::from_sys_days(sd) == d);
    REQUIRE((d + days { 1 }) == mysql::date { 2024, 3, 1 });
    REQUIRE((mysql::date { 2024, 3, 1 } - days { 1 }) == d);
    REQUIRE((mysql::date { 2024, 3, 1 } - d) == days { 1 });
    REQUIRE(mysql::date { 2024, 3, 1 } > d);

    mysql::time t { 1, 2, 3, 456789 };
    REQUIRE(t.is_valid());
    REQUIRE_FALSE(mysql::time { 0, 60, 0, 0 }.is_valid());
    REQUIRE(t.to_duration() == hours { 1 } + minutes { 2 } + seconds { 3 } + microseconds { 456789 });
    REQUIRE(t.total_microseconds() == 3723456789LL);
    REQUIRE(mysql::time::from_duration(t.to_duration()) == t);
    REQUIRE(t.to_string() == "01:02:03.456789");
    REQUIRE(mysql::time { 1, 2, 3, 0 }.to_string() == "01:02:03");
    auto t2 = mysql::time::from_string("01:02:03.456789");
    REQUIRE(t2.has_value());
    REQUIRE(*t2 == t);
    auto t3 = mysql::time::from_string("01:02:03.5");
    REQUIRE(t3.has_value());
    REQUIRE(t3->microsecond == 500000);
    REQUIRE_FALSE(mysql::time::from_string("01:61:00").has_value());

    auto tn = mysql::time::from_duration(microseconds { -3723456789 });
    REQUIRE(tn.negative);
    REQUIRE(tn.hour == 1);
    REQUIRE(tn.minute == 2);
    REQUIRE(tn.second == 3);
    REQUIRE(tn.microsecond == 456789);
    REQUIRE(tn.to_duration() == microseconds { -3723456789 });
    REQUIRE(tn.total_microseconds() == -3723456789LL);
    REQUIRE(tn.to_string() == "-01:02:03.456789");
    auto tn2 = mysql::time::from_string("-01:02:03.456789");
    REQUIRE(tn2.has_value());
    REQUIRE(*tn2 == tn);
    REQUIRE(tn < t);
    REQUIRE_FALSE(mysql::time::from_string("-01:61:00").has_value());

    mysql::datetime dt { 2024, 1, 15, 12, 34, 56, 123456 };
    REQUIRE(dt.is_valid());
    auto tp = dt.to_time_point();
    REQUIRE(mysql::datetime::from_time_point(tp) == dt);
    REQUIRE(dt.to_string() == "2024-01-15 12:34:56.123456");
    auto dt2 = mysql::datetime::from_string("2024-01-15 12:34:56.123456");
    REQUIRE(dt2.has_value());
    REQUIRE(*dt2 == dt);
    auto dt3 = mysql::datetime::from_string("2024-01-15 12:34:56");
    REQUIRE(dt3.has_value());
    REQUIRE(dt3->microsecond == 0);
    REQUIRE_FALSE(mysql::datetime::from_string("2024-01-15T12:34:56").has_value());
    REQUIRE((mysql::datetime { 2024, 1, 15, 12, 34, 57 } - dt) == microseconds { 876544 });
    REQUIRE(mysql::datetime { 2024, 1, 15, 12, 34, 57 } > dt);

    std::hash<mysql::date> hd;
    REQUIRE(hd(d) == hd(mysql::date { 2024, 2, 29 }));
    REQUIRE(hd(d) != hd(mysql::date { 2024, 3, 1 }));
    std::hash<mysql::datetime> hdt;
    REQUIRE(hdt(dt) == hdt(mysql::datetime { 2024, 1, 15, 12, 34, 56, 123456 }));

    std::hash<mysql::time> ht;
    mysql::time z0 { 0, 0, 0, 0, false };
    mysql::time z1 { 0, 0, 0, 0, true };
    REQUIRE(z0 == z1);
    REQUIRE(ht(z0) == ht(z1));
}

TEST_CASE("db: temporal & narrow error handling", "[db][unit]")
{
    using namespace std::chrono;

    // date 非法 → to_sys_days / 算术抛异常
    REQUIRE_THROWS_AS((mysql::date { 2024, 2, 30 }).to_sys_days(), std::runtime_error);
    REQUIRE_THROWS_AS((mysql::date { 2024, 13, 1 }).to_sys_days(), std::runtime_error);
    REQUIRE_THROWS_AS((mysql::date { 0, 0, 0 }).to_sys_days(), std::runtime_error);
    REQUIRE_THROWS_AS((mysql::date { 2024, 2, 30 }) + days { 1 }, std::runtime_error);
    REQUIRE_THROWS_AS((mysql::date { 2024, 2, 30 }) - (mysql::date { 2024, 2, 28 }), std::runtime_error);

    // date::from_string 各种非法
    REQUIRE_FALSE(mysql::date::from_string("").has_value());
    REQUIRE_FALSE(mysql::date::from_string("2024-02").has_value());
    REQUIRE_FALSE(mysql::date::from_string("2024-02-30").has_value());
    REQUIRE_FALSE(mysql::date::from_string("2024-13-01").has_value());
    REQUIRE_FALSE(mysql::date::from_string("2024-00-10").has_value());
    REQUIRE_FALSE(mysql::date::from_string("2024-01-00").has_value());
    REQUIRE_FALSE(mysql::date::from_string("abcd-ef-gh").has_value());
    REQUIRE_FALSE(mysql::date::from_string("20240229").has_value());
    REQUIRE_FALSE(mysql::date::from_string("2024/02/29").has_value());
    REQUIRE_FALSE(mysql::date::from_string("2024--02-29").has_value());

    // time::from_string 各种非法
    REQUIRE_FALSE(mysql::time::from_string("").has_value());
    REQUIRE_FALSE(mysql::time::from_string("12:34").has_value());
    REQUIRE_FALSE(mysql::time::from_string("12:60:00").has_value());
    REQUIRE_FALSE(mysql::time::from_string("12:34:60").has_value());
    REQUIRE_FALSE(mysql::time::from_string("12:34:56.1234567").has_value());
    REQUIRE_FALSE(mysql::time::from_string("1a:00:00").has_value());
    REQUIRE_FALSE(mysql::time::from_string("-").has_value());
    REQUIRE_FALSE(mysql::time::from_string("--01:02:03").has_value());

    // datetime 非法 → is_valid / to_time_point
    REQUIRE_FALSE(mysql::datetime { 2024, 2, 30, 12, 0, 0 }.is_valid());
    REQUIRE_FALSE(mysql::datetime { 2024, 1, 1, 24, 0, 0 }.is_valid());
    REQUIRE_FALSE(mysql::datetime { 2024, 1, 1, 12, 60, 0 }.is_valid());
    REQUIRE_FALSE(mysql::datetime { 2024, 1, 1, 12, 0, 0, 1000000 }.is_valid());
    REQUIRE_THROWS_AS((mysql::datetime { 2024, 2, 30, 12, 0, 0 }).to_time_point(), std::runtime_error);
    REQUIRE_THROWS_AS((mysql::datetime { 2024, 1, 1, 12, 60, 0 }).to_time_point(), std::runtime_error);
    REQUIRE_THROWS_AS((mysql::datetime { 2024, 1, 1, 12, 0, 60 }).to_time_point(), std::runtime_error);

    // datetime::from_string 各种非法
    REQUIRE_FALSE(mysql::datetime::from_string("").has_value());
    REQUIRE_FALSE(mysql::datetime::from_string("2024-01-15").has_value());
    REQUIRE_FALSE(mysql::datetime::from_string("2024-01-15T12:34:56").has_value());
    REQUIRE_FALSE(mysql::datetime::from_string("2024-02-30 12:00:00").has_value());
    REQUIRE_FALSE(mysql::datetime::from_string("2024-01-15 12:60:00").has_value());
    REQUIRE_FALSE(mysql::datetime::from_string("2024-01-15 25:00:00").has_value());
    REQUIRE_FALSE(mysql::datetime::from_string("2024-01-15 -12:00:00").has_value());

    // narrow_int / narrow_uint 越界
    REQUIRE(mysql::detail::narrow_int<int>(std::optional<int64_t> { 12345 }) == 12345);
    REQUIRE_FALSE(mysql::detail::narrow_int<int>(std::nullopt).has_value());
    REQUIRE_THROWS_AS(mysql::detail::narrow_int<int>(std::optional<int64_t> { INT64_MAX }), std::runtime_error);
    REQUIRE_THROWS_AS(mysql::detail::narrow_int<int>(std::optional<int64_t> { INT64_MIN }), std::runtime_error);
    REQUIRE_THROWS_AS(mysql::detail::narrow_int<short>(std::optional<int64_t> { 70000 }), std::runtime_error);
    REQUIRE_THROWS_AS(mysql::detail::narrow_uint<unsigned short>(std::optional<uint64_t> { 70000 }), std::runtime_error);
    REQUIRE_THROWS_AS(mysql::detail::narrow_uint<unsigned>(std::optional<uint64_t> { UINT64_MAX }), std::runtime_error);
    REQUIRE(mysql::detail::narrow_uint<unsigned>(std::optional<uint64_t> { 4000000000ull }) == 4000000000u);
}

#else
#include <catch2/catch_test_macros.hpp>
TEST_CASE("db: skipped", "[db]") { SKIP("HTTPLIB_ENABLED_DATABASE not enabled"); }
#endif
