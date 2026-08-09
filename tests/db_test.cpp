#ifdef HTTPLIB_ENABLED_DATABASE
#include "common.hpp"
#include "db/db_result_impl.h"
#include "httplib/config.hpp"
#include "httplib/db/db_config.hpp"
#include "httplib/db/db_pool.hpp"
#include "httplib/db/db_result.hpp"
#include "httplib/db/db_session.hpp"
#include "httplib/server/middleware/db_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/mysql.hpp>
#include <catch2/catch_test_macros.hpp>

namespace db = httplib::db;
namespace net = httplib::net;

namespace
{

    db::db_config
    make_config()
    {
        db::db_config config;
        config.user = "root";
        config.password = "123456";
        return config;
    }

} // namespace

TEST_CASE("db_result: basics", "[db]")
{
    db::db_result r;
    REQUIRE(r.empty());
    REQUIRE(r.row_count() == 0);
    REQUIRE(r.affected_rows() == 0);
    REQUIRE(r.last_insert_id() == 0);
    REQUIRE(r.warning_count() == 0);
    REQUIRE_THROWS_AS(r.column_index("any"), std::runtime_error);
}

TEST_CASE("db_config: defaults", "[db]")
{
    db::db_config c;
    REQUIRE(c.host == "127.0.0.1");
    REQUIRE(c.port == 3306);
    REQUIRE(c.min_connections == 2);
    REQUIRE(c.max_connections == 16);
    REQUIRE(c.connect_timeout == std::chrono::seconds(5));
    REQUIRE(c.ping_interval == std::chrono::seconds(30));
}

TEST_CASE("db: direct connection", "[db][integration]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();

    net::co_spawn(
        ex,
        [ex, cfg = make_config()]() mutable -> net::awaitable<void>
        {
            boost::mysql::any_connection conn(ex);
            boost::mysql::connect_params params;
            params.server_address.emplace_host_and_port(cfg.host, cfg.port);
            params.username = cfg.user;
            params.password = cfg.password;
            co_await conn.async_connect(params, boost::asio::use_awaitable);
            conn.set_meta_mode(boost::mysql::metadata_mode::full);

            boost::mysql::results result;
            co_await conn.async_execute("SELECT 1 AS val", result, boost::asio::use_awaitable);
            REQUIRE(result.has_value());
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

// ===== Full type coverage =====

TEST_CASE("db: connection_pool direct", "[db][integration]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();
    auto cfg = make_config();

    net::co_spawn(
        ex,
        [ex, cfg]() -> net::awaitable<void>
        {
            boost::mysql::pool_params pparams;
            pparams.server_address.emplace_host_and_port(cfg.host, cfg.port);
            pparams.username = cfg.user;
            pparams.password = cfg.password;
            pparams.initial_size = 1;
            pparams.max_size = 4;
            pparams.connect_timeout = cfg.connect_timeout;

            boost::mysql::connection_pool pool(ex, std::move(pparams));
            net::co_spawn(
                ex,
                [&pool]() -> net::awaitable<void> { co_await pool.async_run(boost::asio::use_awaitable); },
                [](std::exception_ptr e)
                {
                    if (e)
                    {
                        std::rethrow_exception(e);
                    }
                });

            auto conn = co_await pool.async_get_connection(boost::asio::use_awaitable);
            boost::mysql::results r;
            co_await conn->async_execute("SELECT 'pool_ok' AS msg", r, boost::asio::use_awaitable);

            REQUIRE(r.rows().at(0).at(0).as_string() == "pool_ok");
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

TEST_CASE("db: into() extraction", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;
    auto ex = ioc.get_executor();

    net::co_spawn(
        ex,
        [&]() -> net::awaitable<void>
        {
            db::db_pool dbpool(ex, cfg);
            dbpool.start();
            auto sess = co_await dbpool.get_session();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_into (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_into");
            co_await sess.query("INSERT INTO __httplib_into VALUES (42, 'alice')");

            int64_t id = 0;
            std::string name;

            co_await sess.stmt("SELECT id, name FROM __httplib_into").into(id, "id").into(name, "name").execute();

            REQUIRE(id == 42);
            REQUIRE(name == "alice");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_into");
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

TEST_CASE("db: named parameters", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::db_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.get_session();

            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_named (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_named");
            co_await sess.query("INSERT INTO __httplib_named VALUES (77, 'named_test')");

            int64_t id = 0;
            std::string name;

            co_await sess.stmt("SELECT id, name FROM __httplib_named WHERE name = :name")
                .bind("name", "named_test")
                .into(id, "id")
                .into(name, "name")
                .execute();

            REQUIRE(id == 77);
            REQUIRE(name == "named_test");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_named");
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

TEST_CASE("db: statement caching", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::db_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.get_session();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_cache (id INT)");
            co_await sess.query("DELETE FROM __httplib_cache");
            co_await sess.query("INSERT INTO __httplib_cache VALUES (1)");
            co_await sess.query("INSERT INTO __httplib_cache VALUES (2)");

            auto stmt = sess.stmt("SELECT id FROM __httplib_cache WHERE id = ?");

            int64_t id1 = 0;
            co_await stmt.bind(1).into(id1, 0).execute();
            REQUIRE(id1 == 1);

            int64_t id2 = 0;
            co_await stmt.bind(2).into(id2, 0).execute();
            REQUIRE(id2 == 2);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_cache");
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

TEST_CASE("db: all types roundtrip", "[db][integration]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();
    auto cfg = make_config();

    net::co_spawn(
        ex,
        [ex, cfg = std::move(cfg)]() -> net::awaitable<void>
        {
            boost::mysql::any_connection conn(ex);
            boost::mysql::connect_params params;
            params.server_address.emplace_host_and_port(cfg.host, cfg.port);
            params.username = cfg.user;
            params.password = cfg.password;
            co_await conn.async_connect(params, boost::asio::use_awaitable);

            // setup
            {
                boost::mysql::results r;
                co_await conn.async_execute("CREATE DATABASE IF NOT EXISTS test", r, boost::asio::use_awaitable);
                co_await conn.async_execute("USE test", r, boost::asio::use_awaitable);
                co_await conn.async_execute("DROP TABLE IF EXISTS __httplib_types", r, boost::asio::use_awaitable);
                co_await conn.async_execute("CREATE TABLE __httplib_types ("
                                            "  i64 BIGINT,"
                                            "  u64 BIGINT UNSIGNED,"
                                            "  f64 DOUBLE,"
                                            "  str VARCHAR(100),"
                                            "  bin BLOB,"
                                            "  dt_date DATE,"
                                            "  dt_dt DATETIME,"
                                            "  dt_ts TIMESTAMP NULL,"
                                            "  dt_t TIME"
                                            ")",
                                            r,
                                            boost::asio::use_awaitable);
            }

            // insert a value row
            {
                boost::mysql::results ins;
                co_await conn.async_execute("INSERT INTO __httplib_types VALUES (42, 99, 3.14, 'hello', 'world', "
                                            "'2024-01-15', '2024-06-20 18:30:45', '2024-06-20 18:30:45', '12:34:56')",
                                            ins,
                                            boost::asio::use_awaitable);
                REQUIRE(ins.affected_rows() == 1);
            }

            // insert a NULL row
            {
                boost::mysql::results ins;
                co_await conn.async_execute("INSERT INTO __httplib_types VALUES "
                                            "(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)",
                                            ins,
                                            boost::asio::use_awaitable);
            }

            // set meta mode before querying
            conn.set_meta_mode(boost::mysql::metadata_mode::full);

            // query both rows
            auto result_impl = std::make_unique<db::db_result::impl>();
            co_await conn.async_execute("SELECT * FROM __httplib_types ORDER BY i64 IS NULL, i64",
                                        result_impl->data,
                                        boost::asio::use_awaitable);
            build_result_impl(*result_impl);
            db::db_result r(std::move(result_impl));

            REQUIRE(r.row_count() == 2);
            REQUIRE(r.column_count() == 9);
            REQUIRE(r.column_name(0) == "i64");
            REQUIRE(r.column_name(1) == "u64");
            REQUIRE(r.column_name(2) == "f64");
            REQUIRE(r.column_name(3) == "str");
            REQUIRE(r.column_name(4) == "bin");
            REQUIRE(r.column_index("dt_date") == 5);
            REQUIRE(r.column_index("dt_dt") == 6);
            REQUIRE(r.column_index("dt_ts") == 7);
            REQUIRE(r.column_index("dt_t") == 8);

            // ===== row 0: values row =====
            auto row0 = r[0];

            // operator[]
            REQUIRE(row0["i64"] == "42");
            REQUIRE(row0["str"] == "hello");

            // correct type access
            REQUIRE(row0.as_int64("i64") == 42);
            REQUIRE(row0.as_int64(0) == 42);
            REQUIRE(row0.as_uint64("u64") == 99);
            REQUIRE(row0.as_uint64(1) == 99);
            REQUIRE(row0.as_double("f64") == 3.14);
            REQUIRE(row0.as_double(2) == 3.14);
            REQUIRE(row0.as_float("f64") == 3.14f);
            REQUIRE(row0.as_string("str") == "hello");
            REQUIRE(row0.as_string(3) == "hello");
            REQUIRE(row0.as_bool("i64") == true);
            REQUIRE(row0.as_blob("bin").size() == 5);

            // date / datetime / time
            auto d = row0.as_date("dt_date");
            REQUIRE(d.year == 2024);
            REQUIRE(d.month == 1);
            REQUIRE(d.day == 15);

            auto dt = row0.as_datetime("dt_dt");
            REQUIRE(dt.year == 2024);
            REQUIRE(dt.month == 6);
            REQUIRE(dt.day == 20);
            REQUIRE(dt.hour == 18);
            REQUIRE(dt.minute == 30);
            REQUIRE(dt.second == 45);

            // timestamp from DATETIME (no timezone conversion)
            auto ts = row0.as_timestamp("dt_dt");
            REQUIRE(ts > 0);
            REQUIRE(ts < 4102444800LL); // within year 2100

            auto dur = row0.as_duration("dt_t");
            REQUIRE(std::chrono::duration_cast<std::chrono::seconds>(dur).count() == 45296);

            // is_null
            REQUIRE_FALSE(row0.is_null("i64"));
            REQUIRE_FALSE(row0.is_null("str"));

            // ===== row 1: NULLs row =====
            auto row1 = r[1];
            REQUIRE(row1.is_null("i64"));
            REQUIRE(row1.is_null("u64"));
            REQUIRE(row1.is_null("str"));
            REQUIRE(row1.is_null("dt_date"));

            // NULL → throw
            REQUIRE_THROWS_AS(row1.as_int64("i64"), std::runtime_error);
            REQUIRE_THROWS_AS(row1.as_uint64("u64"), std::runtime_error);
            REQUIRE_THROWS_AS(row1.as_double("f64"), std::runtime_error);
            REQUIRE_THROWS_AS(row1.as_string("str"), std::runtime_error);
            REQUIRE_THROWS_AS(row1.as_bool("i64"), std::runtime_error);
            REQUIRE_THROWS_AS(row1.as_date("dt_date"), std::runtime_error);
            REQUIRE_THROWS_AS(row1.as_blob("bin"), std::runtime_error);
            REQUIRE_THROWS_AS(row1[0], std::runtime_error);

            // NULL with default
            REQUIRE(row1.as_int64("i64", -1) == -1);
            REQUIRE(row1.as_uint64("u64", 777) == 777);
            REQUIRE(row1.as_double("f64", 9.9) == 9.9);
            REQUIRE(row1.as_float("f64", 1.5f) == 1.5f);
            REQUIRE(row1.as_string("str", "n/a") == "n/a");
            REQUIRE(row1.as_bool("i64", true) == true);
            REQUIRE(row1.as_bool("i64", false) == false);
            REQUIRE(row1.as_timestamp("dt_ts", 42) == 42);

            // ===== column_type =====
            REQUIRE(r.column_type(0) == db::column_type::int64);
            REQUIRE(r.column_type(1) == db::column_type::uint64);
            REQUIRE(r.column_type(2) == db::column_type::double_);
            REQUIRE(r.column_type(3) == db::column_type::string);
            REQUIRE(r.column_type(4) == db::column_type::blob);
            REQUIRE(r.column_type(5) == db::column_type::date);
            REQUIRE(r.column_type(6) == db::column_type::datetime);
            REQUIRE(r.column_type(7) == db::column_type::datetime);
            REQUIRE(r.column_type(8) == db::column_type::time);

            // ===== row::get<T> =====
            {
                auto row = r[0];
                REQUIRE(row.get<int64_t>("i64") == 42);
                REQUIRE(row.get<uint64_t>("u64") == 99);
                REQUIRE(row.get<double>("f64") == 3.14);
                REQUIRE(row.get<float>("f64") == 3.14f);
                REQUIRE(row.get<bool>("i64") == true);
                REQUIRE(row.get<std::string>("str") == "hello");
                REQUIRE(row.get<db::db_date>("dt_date").year == 2024);
                REQUIRE(row.get<db::db_datetime>("dt_dt").year == 2024);
                auto dur = row.get<std::chrono::microseconds>("dt_t");
                REQUIRE(std::chrono::duration_cast<std::chrono::seconds>(dur).count() == 45296);

                // get<T> with default for NULL
                auto row1 = r[1];
                REQUIRE(row1.get<int64_t>("i64", -1) == -1);
                REQUIRE(row1.get<std::string>("str", "n/a") == "n/a");
                REQUIRE_THROWS_AS(row1.get<int64_t>("i64"), std::runtime_error);
            }

            // cleanup
            {
                boost::mysql::results r;
                co_await conn.async_execute("DROP TABLE IF EXISTS __httplib_types", r, boost::asio::use_awaitable);
            }

            // ===== type mismatch → throw =====
            REQUIRE_THROWS_AS(row0.as_int64("str"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_double("str"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_date("i64"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_datetime("i64"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_duration("i64"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_timestamp("str"), std::runtime_error);

            // column not found → throw via column_index
            REQUIRE_THROWS_AS(row0["no_such_col"], std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_int64("no_such_col"), std::runtime_error);
            REQUIRE_THROWS_AS(r.column_index("no_such_col"), std::runtime_error);

            // ===== iterator =====
            {
                std::vector<std::string> strs;
                for (auto row : r)
                {
                    strs.push_back(row.is_null("str") ? "(null)" : row["str"]);
                }
                REQUIRE(strs.size() == 2);
                REQUIRE(strs[0] == "hello");
                REQUIRE(strs[1] == "(null)");
            }
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

TEST_CASE("db: execute() returns db_result", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::db_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.get_session();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_exec (id INT, val VARCHAR(50))");
            co_await sess.query("DELETE FROM __httplib_exec");
            co_await sess.query("INSERT INTO __httplib_exec VALUES (1, 'one')");
            co_await sess.query("INSERT INTO __httplib_exec VALUES (2, 'two')");

            auto result = co_await sess.stmt("SELECT id, val FROM __httplib_exec WHERE id = ?").bind(1).execute();
            REQUIRE(result.row_count() == 1);
            REQUIRE(result[0]["id"] == "1");
            REQUIRE(result[0]["val"] == "one");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_exec");
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

TEST_CASE("db: ping", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::db_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.get_session();
            auto ok = co_await sess.ping();
            REQUIRE(ok);
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

TEST_CASE("db: query_logger", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::db_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.get_session();

            std::string logged_sql;
            size_t logged_rows = 0;
            sess.set_query_logger(
                [&](db::query_log_entry const& e)
                {
                    logged_sql = e.sql;
                    logged_rows = e.row_count;
                });

            co_await sess.query("SELECT 42 AS n");

            REQUIRE(logged_sql.find("SELECT 42") != std::string::npos);
            REQUIRE(logged_rows == 1);
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

TEST_CASE("db: transaction commit", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::db_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.get_session();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_txn (id INT)");
            co_await sess.query("DELETE FROM __httplib_txn");

            {
                auto txn = co_await sess.begin();
                co_await sess.query("INSERT INTO __httplib_txn VALUES (100)");
                co_await txn.commit();
            }

            auto r = co_await sess.query("SELECT id FROM __httplib_txn");
            REQUIRE(r.row_count() == 1);
            REQUIRE(r[0]["id"] == "100");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_txn");
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

TEST_CASE("db: transaction rollback on drop", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::db_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.get_session();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_rollback (id INT)");
            co_await sess.query("DELETE FROM __httplib_rollback");

            {
                auto txn = co_await sess.begin();
                co_await sess.query("INSERT INTO __httplib_rollback VALUES (200)");
                // txn goes out of scope without commit → rollback in destructor
            }

            auto r = co_await sess.query("SELECT id FROM __httplib_rollback");
            REQUIRE(r.row_count() == 0);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_rollback");
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ioc.run();
}

TEST_CASE("db_middleware: throws when no middleware registered", "[db][middleware]")
{
    test_common::test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/db/nomw",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            REQUIRE_THROWS_AS(httplib::server::middleware::get_db_session(req), std::runtime_error);
            resp.set_string_content("ok"sv, "text/plain"sv);
        });

    ts.start();
    auto resp = UNWRAP(ts.client->get("/db/nomw"));
    REQUIRE(resp.result() == http::status::ok);
}

#else
#include <catch2/catch_test_macros.hpp>
TEST_CASE("db: skipped", "[db]") { SKIP("HTTPLIB_ENABLED_DATABASE not enabled"); }
#endif
