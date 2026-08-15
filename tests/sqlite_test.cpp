#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/db/binder.hpp"
#include "httplib/db/connection_pool.hpp"
#include "httplib/db/extractor.hpp"
#include "httplib/db/session.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

namespace db = httplib::db;
namespace net = httplib::net;

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
            db::sqlite_config sc { path };
            db::connection_pool pool(
                ioc.get_executor(),
                p,
                [sc = std::move(sc)](net::any_io_executor ex) -> net::awaitable<std::unique_ptr<db::session>>
                { co_return std::make_unique<db::session>(co_await db::session::connect(ex, sc)); });
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
#endif
