#ifdef HTTPLIB_ENABLED_DATABASE
#include "common.hpp"
#include "httplib/config.hpp"
#include "httplib/mysql/config.hpp"
#include "httplib/mysql/connection_pool.hpp"
#include "httplib/mysql/mysql_exception.hpp"
#include "httplib/mysql/result.hpp"
#include "httplib/mysql/session.hpp"
#include "httplib/server/middleware/data.hpp"
#include "httplib/server/middleware/mysql_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "mysql/result_impl.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/mysql.hpp>
#include <catch2/catch_test_macros.hpp>

namespace mysql = httplib::mysql;
namespace mw = httplib::server::middleware;
namespace net = httplib::net;

namespace
{

    mysql::pool_params
    make_config()
    {
        mysql::pool_params config;
        config.user = "root";
        config.password = "123456";
        return config;
    }

} // namespace

TEST_CASE("mysql_exception: stores error_code and what", "[db]")
{
    auto ec = boost::system::errc::make_error_code(boost::system::errc::permission_denied);
    mysql::mysql_exception ex(ec, "bad sql");
    REQUIRE(ex.code().value() == ec.value());
    REQUIRE(std::string(ex.what()) == "bad sql");
}

TEST_CASE("result: basics", "[db]")
{
    mysql::result r;
    REQUIRE(r.empty());
    REQUIRE(r.row_count() == 0);
    REQUIRE(r.affected_rows() == 0);
    REQUIRE(r.last_insert_id() == 0);
    REQUIRE(r.warning_count() == 0);
    REQUIRE_THROWS_AS(r.column_index("any"), std::runtime_error);
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
    REQUIRE(p.ping_interval == std::chrono::seconds(30));
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
            mysql::connection_pool dbpool(ex, cfg);
            dbpool.start();
            auto sess = co_await dbpool.async_acquire();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_into (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_into");
            co_await sess.query("INSERT INTO __httplib_into VALUES (42, 'alice')");

            std::optional<int64_t> id;
            std::optional<std::string> name;

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
            mysql::connection_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.async_acquire();

            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_named (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_named");
            co_await sess.query("INSERT INTO __httplib_named VALUES (77, 'named_test')");

            std::optional<int64_t> id;
            std::optional<std::string> name;

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
            mysql::connection_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.async_acquire();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_cache (id INT)");
            co_await sess.query("DELETE FROM __httplib_cache");
            co_await sess.query("INSERT INTO __httplib_cache VALUES (1)");
            co_await sess.query("INSERT INTO __httplib_cache VALUES (2)");

            auto stmt = sess.stmt("SELECT id FROM __httplib_cache WHERE id = ?");

            std::optional<int64_t> id1;
            co_await stmt.bind(1).into(id1, 0).execute();
            REQUIRE(id1 == 1);

            std::optional<int64_t> id2;
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
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connection_pool pool(ioc.get_executor(), cfg);
            pool.start();

            auto sess = co_await pool.async_acquire();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("DROP TABLE IF EXISTS __httplib_types");
            co_await sess.query("CREATE TABLE __httplib_types ("
                                "  i64 BIGINT,"
                                "  u64 BIGINT UNSIGNED,"
                                "  f64 DOUBLE,"
                                "  str VARCHAR(100),"
                                "  bin BLOB,"
                                "  dt_date DATE,"
                                "  dt_dt DATETIME,"
                                "  dt_ts TIMESTAMP NULL,"
                                "  dt_t TIME"
                                ")");

            co_await sess.query("INSERT INTO __httplib_types VALUES (42, 99, 3.14, 'hello', 'world', "
                                "'2024-01-15', '2024-06-20 18:30:45', '2024-06-20 18:30:45', '12:34:56')");
            co_await sess.query("INSERT INTO __httplib_types VALUES "
                                "(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)");

            auto r = co_await sess.query("SELECT * FROM __httplib_types ORDER BY i64 IS NULL, i64");

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

            REQUIRE(*row0.as_int64("i64") == 42);
            REQUIRE(*row0.as_string("str") == "hello");

            // correct type access
            REQUIRE(*row0.as_int64("i64") == 42);
            REQUIRE(*row0.as_int64(0) == 42);
            REQUIRE(*row0.as_uint64("u64") == 99);
            REQUIRE(*row0.as_uint64(1) == 99);
            REQUIRE(*row0.as_double("f64") == 3.14);
            REQUIRE(*row0.as_double(2) == 3.14);
            REQUIRE(*row0.as_float("f64") == 3.14f);
            REQUIRE(*row0.as_string("str") == "hello");
            REQUIRE(*row0.as_string(3) == "hello");
            REQUIRE(*row0.as_bool("i64") == true);
            REQUIRE(row0.as_blob("bin")->size() == 5);

            // date / datetime / time
            auto d = *row0.as_date("dt_date");
            REQUIRE(d.year == 2024);
            REQUIRE(d.month == 1);
            REQUIRE(d.day == 15);

            auto dt = *row0.as_datetime("dt_dt");
            REQUIRE(dt.year == 2024);
            REQUIRE(dt.month == 6);
            REQUIRE(dt.day == 20);
            REQUIRE(dt.hour == 18);
            REQUIRE(dt.minute == 30);
            REQUIRE(dt.second == 45);

            // timestamp from DATETIME (no timezone conversion)
            auto ts = *row0.as_timestamp("dt_dt");
            REQUIRE(ts > std::chrono::system_clock::from_time_t(0));
            REQUIRE(ts < std::chrono::system_clock::from_time_t(4102444800LL)); // within year 2100

            auto t = *row0.as_time("dt_t");
            REQUIRE(t.hour == 12);
            REQUIRE(t.minute == 34);
            REQUIRE(t.second == 56);

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
            REQUIRE(!row1.as_int64("i64").has_value());
            REQUIRE(!row1.as_uint64("u64").has_value());
            REQUIRE(!row1.as_double("f64").has_value());
            REQUIRE(!row1.as_string("str").has_value());
            REQUIRE(!row1.as_bool("i64").has_value());
            REQUIRE(!row1.as_date("dt_date").has_value());
            REQUIRE(!row1.as_blob("bin").has_value());
            REQUIRE(!row1.as_string(0).has_value());

            // NULL with default
            REQUIRE(row1.as_int64("i64").value_or(-1) == -1);
            REQUIRE(row1.as_uint64("u64").value_or(777) == 777);
            REQUIRE(row1.as_double("f64").value_or(9.9) == 9.9);
            REQUIRE(row1.as_float("f64").value_or(1.5f) == 1.5f);
            REQUIRE(row1.as_string("str").value_or("n/a") == "n/a");
            REQUIRE(row1.as_bool("i64").value_or(true) == true);
            REQUIRE(row1.as_bool("i64").value_or(false) == false);
            REQUIRE(row1.as_timestamp("dt_ts").value_or(std::chrono::system_clock::from_time_t(42))
                    == std::chrono::system_clock::from_time_t(42));

            // ===== column_type =====
            REQUIRE(r.column_type(0) == mysql::column_type::int64);
            REQUIRE(r.column_type(1) == mysql::column_type::uint64);
            REQUIRE(r.column_type(2) == mysql::column_type::double_);
            REQUIRE(r.column_type(3) == mysql::column_type::string);
            REQUIRE(r.column_type(4) == mysql::column_type::blob);
            REQUIRE(r.column_type(5) == mysql::column_type::date);
            REQUIRE(r.column_type(6) == mysql::column_type::datetime);
            REQUIRE(r.column_type(7) == mysql::column_type::datetime);
            REQUIRE(r.column_type(8) == mysql::column_type::time);

            // ===== row::get<T> =====
            {
                auto row = r[0];
                REQUIRE(*row.get<int64_t>("i64") == 42);
                REQUIRE(*row.get<uint64_t>("u64") == 99);
                REQUIRE(*row.get<double>("f64") == 3.14);
                REQUIRE(*row.get<float>("f64") == 3.14f);
                REQUIRE(*row.get<bool>("i64") == true);
                REQUIRE(*row.get<std::string>("str") == "hello");
                REQUIRE(row.get<mysql::date>("dt_date")->year == 2024);
                REQUIRE(row.get<mysql::datetime>("dt_dt")->year == 2024);
                auto t = *row.get<mysql::time>("dt_t");
                REQUIRE(t.hour == 12);

                // get<T> with default for NULL
                auto row1 = r[1];
                REQUIRE(row1.get<int64_t>("i64").value_or(-1) == -1);
                REQUIRE(row1.get<std::string>("str").value_or("n/a") == "n/a");
                REQUIRE_THROWS_AS(row1.get<int64_t>("i64"), std::runtime_error);
            }

            // ===== type mismatch → throw =====
            REQUIRE_THROWS_AS(row0.as_int64("str"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_double("str"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_date("i64"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_datetime("i64"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_time("i64"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_timestamp("str"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_json("i64"), std::runtime_error);

            // column not found → throw via column_index
            REQUIRE_THROWS_AS(row0.as_string("no_such_col"), std::runtime_error);
            REQUIRE_THROWS_AS(row0.as_int64("no_such_col"), std::runtime_error);
            REQUIRE_THROWS_AS(r.column_index("no_such_col"), std::runtime_error);

            // ===== iterator =====
            {
                std::vector<std::string> strs;
                for (auto row : r)
                {
                    strs.push_back(row.is_null("str") ? "(null)" : std::string(*row.as_string("str")));
                }
                REQUIRE(strs.size() == 2);
                REQUIRE(strs[0] == "hello");
                REQUIRE(strs[1] == "(null)");
            }

            co_await sess.query("DROP TABLE IF EXISTS __httplib_types");
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

TEST_CASE("db: execute() returns result", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connection_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.async_acquire();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_exec (id INT, val VARCHAR(50))");
            co_await sess.query("DELETE FROM __httplib_exec");
            co_await sess.query("INSERT INTO __httplib_exec VALUES (1, 'one')");
            co_await sess.query("INSERT INTO __httplib_exec VALUES (2, 'two')");

            auto result = co_await sess.stmt("SELECT id, val FROM __httplib_exec WHERE id = ?").bind(1).execute();
            REQUIRE(result.row_count() == 1);
            REQUIRE(result[0].as_int64("id") == 1);
            REQUIRE(*result[0].as_string("val") == "one");

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

TEST_CASE("db: multi-statement query", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connection_pool pool(ioc.get_executor(), cfg);
            pool.start();

            auto sess = co_await pool.async_acquire();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_batch (id INT, val VARCHAR(50))");
            co_await sess.query("DELETE FROM __httplib_batch");

            auto r = co_await sess.query("INSERT INTO __httplib_batch VALUES (1, 'a');"
                                         "INSERT INTO __httplib_batch VALUES (2, 'b');"
                                         "SELECT * FROM __httplib_batch ORDER BY id");

            REQUIRE(r.row_count() == 2);
            REQUIRE(r[0].as_int64("id") == 1);
            REQUIRE(*r[1].as_string("val") == "b");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_batch");
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

TEST_CASE("db: json column", "[db][integration]")
{
    auto cfg = make_config();
    cfg.min_connections = 1;
    cfg.max_connections = 4;

    net::io_context ioc;

    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            mysql::connection_pool pool(ioc.get_executor(), cfg);
            pool.start();

            auto sess = co_await pool.async_acquire();
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_json (id INT, data JSON)");
            co_await sess.query("DELETE FROM __httplib_json");
            co_await sess.query("INSERT INTO __httplib_json VALUES (1, '{\"x\":1,\"y\":\"hi\"}')");

            auto r = co_await sess.query("SELECT data FROM __httplib_json WHERE id = 1");
            auto val = *r[0].as_json("data");
            REQUIRE(val.as_object().at("x").as_int64() == 1);
            REQUIRE(val.as_object().at("y").as_string() == "hi");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_json");
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
            mysql::connection_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.async_acquire();
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
            mysql::connection_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.async_acquire();

            std::string logged_sql;
            size_t logged_rows = 0;
            sess.set_query_logger(
                [&](mysql::query_log_entry const& e)
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
            mysql::connection_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.async_acquire();
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
            REQUIRE(*r[0].as_int64("id") == 100);

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
            mysql::connection_pool dbpool(ioc.get_executor(), cfg);
            dbpool.start();

            auto sess = co_await dbpool.async_acquire();
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

TEST_CASE("mysql_middleware: throws when no middleware registered", "[db][middleware]")
{
    test_common::test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>("/db/nomw",
                                                  [](httplib::server::request& req, httplib::server::response& resp)
                                                  {
                                                      REQUIRE_THROWS_AS(mw::fetch<mw::mysql_middleware>(req),
                                                                        std::exception);
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
