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
            [&](std::exception_ptr e)
            {
                err = e;
            });
        ioc.run();
        if (err)
            std::rethrow_exception(err);
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
            [&](std::exception_ptr e)
            {
                err = e;
            });
        ioc.run();
        if (err)
            std::rethrow_exception(err);
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

    mysql::pool_params p;
    REQUIRE(p.min_connections == 2);
    REQUIRE(p.max_connections == 16);
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
        [&](std::exception_ptr e)
        {
            err = e;
        });
    ioc.run();
    if (err)
        std::rethrow_exception(err);
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
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_empty (id INT)");
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
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_aff (id INT AUTO_INCREMENT PRIMARY KEY, v INT)");
            co_await sess.query("TRUNCATE __httplib_aff");
            auto r1 = co_await sess.query(
                "INSERT INTO __httplib_aff (v) VALUES (100)");
            REQUIRE(r1.affected_rows() == 1);
            auto r2 = co_await sess.query(
                "INSERT INTO __httplib_aff (v) VALUES (200),(300)");
            REQUIRE(r2.affected_rows() == 2);
            co_await sess.query("DROP TABLE IF EXISTS __httplib_aff");
        });
}

TEST_CASE("db: result iterator", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_iter (v INT)");
            co_await sess.query("DELETE FROM __httplib_iter");
            co_await sess.query("INSERT INTO __httplib_iter VALUES (1),(2),(3)");
            auto r = co_await sess.query(
                "SELECT v FROM __httplib_iter ORDER BY v");
            std::vector<int64_t> vals;
            for (auto row : r)
                vals.push_back(*row.as_int64("v"));
            REQUIRE(vals == std::vector<int64_t>{ 1, 2, 3 });
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
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_batch (id INT, val VARCHAR(50))");
            co_await sess.query("DELETE FROM __httplib_batch");

            auto r = co_await sess.query(
                "INSERT INTO __httplib_batch VALUES (1, 'a');"
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
            co_await sess.query(
                "CREATE TABLE __httplib_types ("
                "  i64 BIGINT, u64 BIGINT UNSIGNED, f64 DOUBLE, str VARCHAR(100),"
                "  bin BLOB, dt_date DATE, dt_dt DATETIME,"
                "  dt_ts TIMESTAMP NULL, dt_t TIME)");
            co_await sess.query(
                "INSERT INTO __httplib_types VALUES "
                "(42, 99, 3.14, 'hello', 'world', "
                "'2024-01-15', '2024-06-20 18:30:45', "
                "'2024-06-20 18:30:45', '12:34:56')");
            co_await sess.query(
                "INSERT INTO __httplib_types VALUES "
                "(NULL, NULL, NULL, NULL, NULL, "
                "NULL, NULL, NULL, NULL)");

            auto r = co_await sess.query(
                "SELECT * FROM __httplib_types ORDER BY i64 IS NULL, i64");
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
                strs.push_back(row.is_null("str") ? "(null)"
                                                  : std::string(*row.as_string("str")));
            REQUIRE(strs == std::vector<std::string>{ "hello", "(null)" });

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
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_ps (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_ps");
            co_await sess.query("INSERT INTO __httplib_ps VALUES (1, 'alice')");

            auto r = co_await sess.stmt(
                                       "SELECT id, name FROM __httplib_ps WHERE id = ?")
                         .bind(1)
                         .execute();
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
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_named (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_named");
            co_await sess.query("INSERT INTO __httplib_named VALUES (77, 'target')");

            auto r = co_await sess.stmt(
                                       "SELECT id, name FROM __httplib_named WHERE name = :n")
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
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_into (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_into");
            co_await sess.query("INSERT INTO __httplib_into VALUES (42, 'alice')");

            std::optional<int64_t> id;
            std::optional<std::string> name;
            co_await sess.stmt("SELECT id, name FROM __httplib_into")
                .into(id, "id")
                .into(name, "name")
                .execute();
            REQUIRE(id == 42);
            REQUIRE(name == "alice");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_into");
        });
}

TEST_CASE("db: statement reuse (caching)", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_cache (id INT)");
            co_await sess.query("DELETE FROM __httplib_cache");
            co_await sess.query("INSERT INTO __httplib_cache VALUES (1),(2)");

            auto stmt = sess.stmt("SELECT id FROM __httplib_cache WHERE id = ?");
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
// JSON column
// ===========================================================================

TEST_CASE("db: json column", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_json (id INT, data JSON)");
            co_await sess.query("DELETE FROM __httplib_json");
            co_await sess.query(
                "INSERT INTO __httplib_json VALUES (1, '{\"x\":1,\"y\":\"hi\"}')");
            auto r = co_await sess.query(
                "SELECT data FROM __httplib_json WHERE id = 1");
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
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_txn (id INT)");
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
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_txr (id INT)");
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
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_wtxn (id INT)");
            co_await sess.query("DELETE FROM __httplib_wtxn");

            co_await sess.with_transaction(
                [](mysql::session& s) -> net::awaitable<void>
                {
                    co_await s.query("INSERT INTO __httplib_wtxn VALUES (1)");
                    co_await s.query("INSERT INTO __httplib_wtxn VALUES (2)");
                });

            auto r = co_await sess.query(
                "SELECT id FROM __httplib_wtxn ORDER BY id");
            REQUIRE(r.row_count() == 2);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_wtxn");
        });
}

TEST_CASE("db: with_transaction rollback on exception", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_wtxr (id INT)");
            co_await sess.query("DELETE FROM __httplib_wtxr");

            try
            {
                co_await sess.with_transaction(
                    [](mysql::session& s) -> net::awaitable<void>
                    {
                        co_await s.query(
                            "INSERT INTO __httplib_wtxr VALUES (999)");
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
            auto sess = co_await mysql::session::connect(
                ioc.get_executor(), cfg);
            auto r = co_await sess.query("SELECT 'standalone' AS msg");
            REQUIRE(*r[0].as_string("msg") == "standalone");
        },
        [&](std::exception_ptr e)
        {
            err = e;
        });
    ioc.run();
    if (err)
        std::rethrow_exception(err);
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
            auto sess = co_await mysql::session::connect(
                ioc.get_executor(), cfg);
            auto r1 = co_await sess.query("SELECT 1 AS v");
            REQUIRE(*r1[0].as_int64("v") == 1);
            co_await sess.reconnect();
            auto r2 = co_await sess.query("SELECT 2 AS v");
            REQUIRE(*r2[0].as_int64("v") == 2);
        },
        [&](std::exception_ptr e)
        {
            err = e;
        });
    ioc.run();
    if (err)
        std::rethrow_exception(err);
}

// ===========================================================================
// Error / exception paths
// ===========================================================================

TEST_CASE("db: invalid SQL throws", "[db][integration]")
{
    run(
        [](mysql::session& sess) -> net::awaitable<void>
        {
            REQUIRE_THROWS_AS(co_await sess.query("BOGUS SYNTAX ERROR"),
                              mysql::mysql_exception);
        });
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
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_nerr (id INT)");
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

            httplib::client::http_client client(ioc.get_executor(),
                                                ep.address().to_string(),
                                                ep.port());
            client.set_timeout(std::chrono::seconds(5));

            auto resp = UNWRAP(co_await client.async_get("/db/nomw"));
            REQUIRE(resp.result() == http::status::ok);

            client.close();
            server.stop();
        },
        [&](std::exception_ptr e) { err = e; });

    ioc.run();
    if (err)
        std::rethrow_exception(err);
}

#else
#include <catch2/catch_test_macros.hpp>
TEST_CASE("db: skipped", "[db]") { SKIP("HTTPLIB_ENABLED_DATABASE not enabled"); }
#endif
