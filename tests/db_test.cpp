#ifdef HTTPLIB_ENABLED_DATABASE
#include "common.hpp"
#include "httplib/db/config.hpp"
#include "httplib/db/connection_pool.hpp"
#include "httplib/db/exception.hpp"
#include "httplib/db/options.hpp"
#include "httplib/db/result.hpp"
#include "httplib/db/session.hpp"
#include "httplib/server/middleware/data.hpp"
#include "httplib/server/middleware/db_middleware.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>

namespace db = httplib::db;
namespace mw = httplib::server::middleware;
namespace net = httplib::net;

namespace
{

    db::mysql_config
    make_mysql_cfg()
    {
        db::mysql_config cfg;
        cfg.user = "root";
        cfg.password = "123456";
        return cfg;
    }

    db::pool_params
    make_cfg()
    {
        db::pool_params cfg;
        cfg.min_connections = 1;
        cfg.max_connections = 4;
        return cfg;
    }

    db::connection_pool
    make_pool(net::any_io_executor ex, db::pool_params p = make_cfg(), db::mysql_config mc = make_mysql_cfg())
    {
        return db::make_pool(ex, std::move(p), "mysql", mc.to_options().to_connection_string());
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
                db::connection_pool pool = make_pool(ioc.get_executor());
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
                db::connection_pool pool = make_pool(ioc.get_executor());
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
    db::db_exception ex(ec, "bad sql");
    REQUIRE(ex.code().value() == ec.value());
    REQUIRE(std::string(ex.what()) == "bad sql");
}

TEST_CASE("result: empty default-constructed", "[db]")
{
    db::result r;
    REQUIRE(r.empty());
    REQUIRE(r.row_count() == 0);
    REQUIRE(r.resultset_count() == 0);
    REQUIRE(r.affected_rows() == 0);
    REQUIRE_FALSE(r.next_resultset());
}

TEST_CASE("config: defaults", "[db]")
{
    db::mysql_config c;
    REQUIRE(c.host == "127.0.0.1");
    REQUIRE(c.port == 3306);
    REQUIRE(c.connect_timeout == std::chrono::seconds(5));
    REQUIRE(c.max_cached_statements == 64);
    REQUIRE(c.time_zone.empty());

    db::pool_params p;
    REQUIRE(p.min_connections == 2);
    REQUIRE(p.max_connections == 16);
    REQUIRE_FALSE(p.validate_on_borrow);
}

TEST_CASE("db options: parse connection string", "[db]")
{
    auto o = db::options::parse("host=127.0.0.1 port=3306 user=root password=123456 db=main");
    REQUIRE(o.get_or("host") == "127.0.0.1");
    REQUIRE(*o.as_uint16("port") == 3306);
    REQUIRE(o.get_or("user") == "root");
    REQUIRE(o.get_or("password") == "123456");
    REQUIRE(o.get_or("db") == "main");
    REQUIRE_FALSE(o.has("time_zone"));
    REQUIRE(o.get_or("missing", "default") == "default");

    auto q = db::options::parse("db=\"my db\" time_zone='+08:00' ssl=1 connect_timeout=3");
    REQUIRE(q.get_or("db") == "my db");
    REQUIRE(q.get_or("time_zone") == "+08:00");
    REQUIRE(*q.as_bool("ssl"));
    REQUIRE(*q.as_seconds("connect_timeout") == std::chrono::seconds(3));

    REQUIRE(db::options::parse("").get_or("x", "y") == "y");
    REQUIRE_FALSE(db::options::parse("port=abc").as_uint16("port").has_value());
}

TEST_CASE("db options: parse syntax edge cases", "[db]")
{
    // 重复键：后者覆盖前者
    auto dup = db::options::parse("user=a user=b");
    REQUIRE(dup.get_or("user") == "b");

    // 值含 '='（不加引号，读到空白为止）
    auto eq = db::options::parse("token=sha256:abc=def db=main");
    REQUIRE(eq.get_or("token") == "sha256:abc=def");
    REQUIRE(eq.get_or("db") == "main");

    // 引号内转义：反斜杠转义双引号；未加引号的值里反斜杠原样保留
    auto esc = db::options::parse("db=\"my \\\"db\\\"\" path=a\\b");
    REQUIRE(esc.get_or("db") == "my \"db\"");
    REQUIRE(esc.get_or("path") == "a\\b");

    // 裸键无 '=' → 值为空
    auto bare = db::options::parse("user");
    REQUIRE(bare.has("user"));
    REQUIRE(bare.get_or("user").empty());

    // 显式空值
    auto emptyv = db::options::parse("a= b=c");
    REQUIRE(emptyv.get_or("a").empty());
    REQUIRE(emptyv.get_or("b") == "c");

    // 多余/首尾空白
    auto ws = db::options::parse("   a=1 \t b=2   ");
    REQUIRE(ws.get_or("a") == "1");
    REQUIRE(ws.get_or("b") == "2");

    // 以 '=' 开头的垃圾 token 被忽略
    auto junk = db::options::parse("=1 user=root");
    REQUIRE(junk.get_or("user") == "root");

    // 单引号与双引号等价
    auto sq = db::options::parse("db='a b' x=\"c d\"");
    REQUIRE(sq.get_or("db") == "a b");
    REQUIRE(sq.get_or("x") == "c d");
}

TEST_CASE("db options: typed getters", "[db]")
{
    auto o = db::options::parse("neg=-5 big=99999 small=123 sec=-3 sec0=0 fl=12.5 bad=abc "
                                "b=true y=yes o=on f=false n=no ff=off z=0 u=TRUE");

    REQUIRE(*o.as_int("neg") == -5);
    REQUIRE(*o.as_uint16("small") == 123);
    REQUIRE_FALSE(o.as_uint16("big").has_value()); // 越界
    REQUIRE_FALSE(o.as_uint16("neg").has_value()); // 负数
    REQUIRE_FALSE(o.as_uint16("bad").has_value()); // 非法
    REQUIRE_FALSE(o.as_uint16("missing").has_value());

    REQUIRE(*o.as_seconds("sec") == std::chrono::seconds(-3));
    REQUIRE(*o.as_seconds("sec0") == std::chrono::seconds(0));
    REQUIRE_FALSE(o.as_seconds("fl").has_value()); // 小数
    REQUIRE_FALSE(o.as_seconds("bad").has_value());

    REQUIRE(*o.as_bool("b"));
    REQUIRE(*o.as_bool("y"));
    REQUIRE(*o.as_bool("o"));
    REQUIRE_FALSE(*o.as_bool("f"));
    REQUIRE_FALSE(*o.as_bool("n"));
    REQUIRE_FALSE(*o.as_bool("ff"));
    REQUIRE_FALSE(*o.as_bool("z"));
    REQUIRE_FALSE(o.as_bool("u").has_value()); // 大小写敏感
    REQUIRE_FALSE(o.as_bool("bad").has_value());
    REQUIRE_FALSE(o.as_bool("missing").has_value());

    // get / has / get_or
    REQUIRE_FALSE(o.has("missing"));
    REQUIRE_FALSE(o.get("missing").has_value());
    REQUIRE(o.get_or("missing", "d") == "d");
    REQUIRE(o.has("neg"));
    REQUIRE(*o.get("neg") == "-5");
}

TEST_CASE("db options: to_connection_string round-trip", "[db]")
{
    db::options a;
    a.set("host", "127.0.0.1");
    a.set("port", "3306");
    auto a2 = db::options::parse(a.to_connection_string());
    REQUIRE(a2.get_or("host") == "127.0.0.1");
    REQUIRE(a2.to_connection_string() == a.to_connection_string());

    // 含空格 / '=' / 引号的值 → 序列化加引号，可往返
    db::options b;
    b.set("password", "my pass");
    b.set("db", "a=b");
    b.set("note", "say \"hi\"");
    auto b2 = db::options::parse(b.to_connection_string());
    REQUIRE(b2.get_or("password") == "my pass");
    REQUIRE(b2.get_or("db") == "a=b");
    REQUIRE(b2.get_or("note") == "say \"hi\"");
    REQUIRE(b2.to_connection_string() == b.to_connection_string());

    // 空值
    db::options c;
    c.set("time_zone", "");
    auto c2 = db::options::parse(c.to_connection_string());
    REQUIRE(c2.has("time_zone"));
    REQUIRE(c2.get_or("time_zone").empty());
    REQUIRE(c2.to_connection_string() == c.to_connection_string());

    // 空 options → 空串 → 空 options
    db::options empty;
    REQUIRE(empty.to_connection_string().empty());
    REQUIRE(db::options::parse("").to_connection_string().empty());
}

TEST_CASE("db: connect via connection string", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            std::string conn = make_mysql_cfg().to_options().to_connection_string();
            auto sess = co_await db::session::connect(ioc.get_executor(), "mysql", conn);
            auto r = co_await sess.query("SELECT 1 AS n");
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_int64("n") == 1);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: string connect aliases and defaults", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            // 只给 user/password：host/port 走默认 127.0.0.1:3306
            {
                auto sess = co_await db::session::connect(ioc.get_executor(), "mysql", "user=root password=123456");
                auto r = co_await sess.query("SELECT 1 AS n");
                REQUIRE(r.row_count() == 1);
            }

            auto s0 = co_await db::session::connect(ioc.get_executor(), "mysql", "user=root password=123456");
            co_await s0.query("CREATE DATABASE IF NOT EXISTS test");

            // database= 与 db= 别名等价，且连接后默认库生效
            for (auto const* db_key : { "database", "db" })
            {
                std::string conn = std::string("user=root password=123456 ") + db_key + "=test";
                auto sess = co_await db::session::connect(ioc.get_executor(), "mysql", conn);
                auto r = co_await sess.query("SELECT DATABASE() AS d");
                REQUIRE(*r[0].as_string("d") == "test");
            }

            // 引号值：host 用双引号包裹
            {
                auto sess = co_await db::session::connect(ioc.get_executor(),
                                                          "mysql",
                                                          "host=\"127.0.0.1\" port=3306 user=root password=123456");
                auto r = co_await sess.query("SELECT 1 AS n");
                REQUIRE(r.row_count() == 1);
            }
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: string connect time_zone and charset", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            {
                auto sess = co_await db::session::connect(ioc.get_executor(),
                                                          "mysql",
                                                          "user=root password=123456 time_zone=+00:00");
                auto r = co_await sess.query("SELECT @@session.time_zone AS tz");
                REQUIRE(*r[0].as_string("tz") == "+00:00");
            }
            {
                auto sess = co_await db::session::connect(ioc.get_executor(),
                                                          "mysql",
                                                          "user=root password=123456 charset=utf8mb4");
                auto r = co_await sess.query("SELECT @@character_set_connection AS cs");
                REQUIRE(*r[0].as_string("cs") == "utf8mb4");
            }
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: unknown backend throws", "[db]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            bool thrown = false;
            try
            {
                auto sess = co_await db::session::connect(ioc.get_executor(), "oracle", "user=x");
            }
            catch (db::db_exception const&)
            {
                thrown = true;
            }
            REQUIRE(thrown);
            REQUIRE_THROWS_AS(db::make_pool(ioc.get_executor(), db::pool_params {}, "oracle", "user=x"),
                              db::db_exception);
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: query bind time/datetime/null round-trip", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_temporal "
                                "(id INT, t TIME(6), dt DATETIME(6), n VARCHAR(10))");
            co_await sess.query("DELETE FROM __httplib_temporal");

            // 负 TIME、带微秒的 DATETIME、NULL 都要能无损往返
            auto neg_t = db::time::from_duration(std::chrono::microseconds { -3723456789 });
            db::datetime dt { 2024, 1, 15, 12, 34, 56, 123456 };
            co_await sess.query("INSERT INTO __httplib_temporal VALUES (:id, :t, :dt, :n)",
                                db::bind("id", 1),
                                db::bind("t", neg_t),
                                db::bind("dt", dt),
                                db::bind("n", nullptr));
            auto r = co_await sess.query("SELECT t, dt, n FROM __httplib_temporal WHERE id = 1");

            REQUIRE(*r[0].as_time("t") == neg_t);
            REQUIRE(*r[0].as_datetime("dt") == dt);
            REQUIRE(r[0].is_null("n"));

            co_await sess.query("DROP TABLE IF EXISTS __httplib_temporal");
        });
}

// ===========================================================================
// Connection pool
// ===========================================================================

TEST_CASE("db: connection_pool basics", "[db][integration]")
{
    run_pool(
        [](db::connection_pool& pool) -> net::awaitable<void>
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
            db::connection_pool pool = make_pool(ioc.get_executor(), cfg);
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

TEST_CASE("db: pool honors max_connections over min_connections", "[db][integration]")
{
    // min_connections 不应突破 max_connections 上限
    auto cfg = make_cfg();
    cfg.min_connections = 5;
    cfg.max_connections = 2;

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::connection_pool pool = make_pool(ioc.get_executor(), cfg);
            pool.start();

            // 等待后台预建完成（最多 3s；DB 不可用则 total_count 保持 0，测试不误报）
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (pool.total_count() < cfg.min_connections && std::chrono::steady_clock::now() < deadline)
            {
                net::steady_timer settle(ioc.get_executor());
                settle.expires_after(std::chrono::milliseconds(50));
                co_await settle.async_wait(boost::asio::use_awaitable);
            }

            REQUIRE(pool.total_count() <= cfg.max_connections);
            pool.stop();
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: idle connections are reaped by idle_timeout", "[db][integration]")
{
    // 即使 health_check 周期比 idle_timeout 短，空闲连接也应被 idle_timeout 回收
    auto cfg = make_cfg();
    cfg.min_connections = 1;
    cfg.max_connections = 4;
    cfg.idle_check_interval = std::chrono::seconds(1);
    cfg.idle_timeout = std::chrono::seconds(2);
    cfg.health_check_interval = std::chrono::seconds(1);

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::connection_pool pool = make_pool(ioc.get_executor(), cfg);
            pool.start();

            {
                std::vector<db::connection_pool::session_handle> handles;
                for (int i = 0; i < 3; ++i)
                {
                    handles.push_back(co_await pool.async_acquire());
                }
                REQUIRE(pool.total_count() >= 3);
            } // handles 析构 → 全部归还为 idle

            // 轮询等待 maintenance 回收，最多 10s
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (pool.total_count() > cfg.min_connections && std::chrono::steady_clock::now() < deadline)
            {
                net::steady_timer settle(ioc.get_executor());
                settle.expires_after(std::chrono::milliseconds(100));
                co_await settle.async_wait(boost::asio::use_awaitable);
            }

            REQUIRE(pool.total_count() == cfg.min_connections);
            pool.stop();
        },
        [&](std::exception_ptr e) { err = e; });
    ioc.run();
    if (err)
    {
        std::rethrow_exception(err);
    }
}

TEST_CASE("db: pool survives unreachable server", "[db][integration]")
{
    // 连接失败应被维护协程捕获并记录日志，而不是冒泡到 completion handler 导致崩溃
    auto mc = make_mysql_cfg();
    mc.port = 1; // 无 MySQL 监听
    mc.connect_timeout = std::chrono::seconds(1);
    auto cfg = make_cfg();
    cfg.min_connections = 2;
    cfg.idle_check_interval = std::chrono::seconds(1);

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::connection_pool pool = make_pool(ioc.get_executor(), cfg, mc);
            pool.start();

            // 给 pre-create 失败 + maintenance refill 失败留时间
            net::steady_timer settle(ioc.get_executor());
            settle.expires_after(std::chrono::seconds(2));
            co_await settle.async_wait(boost::asio::use_awaitable);

            REQUIRE(pool.total_count() == 0);
            pool.stop();
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
        [](db::session& sess) -> net::awaitable<void>
        {
            auto r = co_await sess.query("SELECT 42 AS n, 'hi' AS s");
            REQUIRE(r.row_count() == 1);
            REQUIRE(r.column_count() == 2);
            REQUIRE(r.column_name(0) == "n");
            REQUIRE(*r[0].as_int64("n") == 42);
            REQUIRE(*r[0].as_string("s") == "hi");
        });
}

TEST_CASE("db: touch updates last_active on query", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            auto t0 = sess.last_active_time();
            co_await sess.query("SELECT 1");
            auto t1 = sess.last_active_time();
            REQUIRE(t1 > t0);
        });
}

TEST_CASE("db: empty result", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
            REQUIRE(r.column_type(0) == db::column_type::int64);
            REQUIRE(r.column_type(1) == db::column_type::uint64);
            REQUIRE(r.column_type(2) == db::column_type::double_);
            REQUIRE(r.column_type(3) == db::column_type::string);
            REQUIRE(r.column_type(4) == db::column_type::blob);
            REQUIRE(r.column_type(5) == db::column_type::date);
            REQUIRE(r.column_type(6) == db::column_type::datetime);
            REQUIRE(r.column_type(8) == db::column_type::time);

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
            REQUIRE(row0.get<db::date>("dt_date")->year == 2024);

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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_into (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_into");
            co_await sess.query("INSERT INTO __httplib_into VALUES (42, 'alice')");

            std::optional<int64_t> id;
            std::optional<std::string> name;
            co_await sess.stmt("SELECT id, name FROM __httplib_into")
                .execute(db::into(id, "id"), db::into(name, "name"));
            REQUIRE(id == 42);
            REQUIRE(name == "alice");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_into");
        });
}

TEST_CASE("db: query into() extraction", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_qinto (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_qinto");
            co_await sess.query("INSERT INTO __httplib_qinto VALUES (1, 'a'),(2, 'b')");

            std::optional<std::string> name;
            co_await sess.query("SELECT name FROM __httplib_qinto WHERE id = 1", db::into(name, "name"));
            REQUIRE(name == "a");

            std::vector<int64_t> ids;
            co_await sess.query("SELECT id FROM __httplib_qinto ORDER BY id", db::into(ids));
            REQUIRE(ids == std::vector<int64_t> { 1, 2 });

            co_await sess.query("DROP TABLE IF EXISTS __httplib_qinto");
        });
}

TEST_CASE("db: query bind positional", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_qb (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_qb");
            co_await sess.query("INSERT INTO __httplib_qb VALUES (1, 'a'),(2, 'b')");

            std::optional<std::string> name;
            co_await sess.query("SELECT name FROM __httplib_qb WHERE id = :id", db::bind(2), db::into(name, "name"));
            REQUIRE(name == "b");

            co_await sess.query("DROP TABLE IF EXISTS __httplib_qb");
        });
}

TEST_CASE("db: query bind named + escaping", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_qe (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_qe");

            // 单引号、反斜杠、末尾反斜杠、反斜杠+单引号都要能无损往返
            std::vector<std::string> evil = { "a'b", "abc\\", "a\\nb", "a\\'b" };
            for (size_t i = 0; i < evil.size(); ++i)
            {
                co_await sess.query("INSERT INTO __httplib_qe VALUES (:id, :n)",
                                    db::bind("id", static_cast<int64_t>(i)),
                                    db::bind("n", evil[i]));
            }
            auto r = co_await sess.query("SELECT name FROM __httplib_qe ORDER BY id");
            REQUIRE(r.row_count() == evil.size());
            for (size_t i = 0; i < evil.size(); ++i)
            {
                REQUIRE(*r[i].as_string("name") == evil[i]);
            }

            co_await sess.query("DROP TABLE IF EXISTS __httplib_qe");
        });
}

TEST_CASE("db: query bind null and blob", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_qn (id INT, b BLOB)");
            co_await sess.query("DELETE FROM __httplib_qn");

            co_await sess.query("INSERT INTO __httplib_qn VALUES (:id, :b)", db::bind("id", 1), db::bind("b", nullptr));
            auto r1 = co_await sess.query("SELECT b FROM __httplib_qn WHERE id = 1");
            REQUIRE(r1[0].is_null("b"));

            std::byte buf[] = { std::byte { 0x00 }, std::byte { 0x01 }, std::byte { 0xFF } };
            co_await sess.query("INSERT INTO __httplib_qn VALUES (:id, :b)",
                                db::bind("id", 2),
                                db::bind("b", std::span<std::byte const>(buf, 3)));
            auto r2 = co_await sess.query("SELECT b FROM __httplib_qn WHERE id = 2");
            auto blob = r2[0].as_blob("b");
            REQUIRE(blob->size() == 3);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_qn");
        });
}

TEST_CASE("db: query bind scalar types round-trip", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_qs ("
                                "u64 BIGINT UNSIGNED, s SMALLINT, us SMALLINT UNSIGNED, u INT UNSIGNED, "
                                "d DOUBLE, f FLOAT, b TINYINT(1), dt DATE, j JSON, ts TIMESTAMP)");
            co_await sess.query("DELETE FROM __httplib_qs");

            auto tp = std::chrono::sys_days(std::chrono::year(2024) / std::chrono::month(6) / std::chrono::day(1))
                      + std::chrono::hours(12);
            boost::json::value jv {
                { "k", "v" }
            };
            co_await sess.query("INSERT INTO __httplib_qs VALUES (:u64, :s, :us, :u, :d, :f, :b, :dt, :j, :ts)",
                                db::bind("u64", uint64_t { 42 }),
                                db::bind("s", short { -7 }),
                                db::bind("us", static_cast<unsigned short>(7)),
                                db::bind("u", 123u),
                                db::bind("d", 2.25),
                                db::bind("f", 1.5f),
                                db::bind("b", true),
                                db::bind("dt", db::date { 2024, 6, 1 }),
                                db::bind("j", jv),
                                db::bind("ts", tp));

            auto r = co_await sess.query("SELECT u64, s, us, u, d, f, b, dt, j, ts FROM __httplib_qs");
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_uint64("u64") == 42);
            REQUIRE(*r[0].as_int64("s") == -7);
            REQUIRE(*r[0].as_uint64("us") == 7);
            REQUIRE(*r[0].as_uint64("u") == 123);
            REQUIRE(*r[0].as_double("d") == 2.25);
            REQUIRE(*r[0].as_float("f") == 1.5f);
            REQUIRE(*r[0].as_bool("b") == true);
            REQUIRE(*r[0].as_date("dt") == db::date { 2024, 6, 1 });
            REQUIRE(*r[0].as_json("j") == jv);
            REQUIRE(*r[0].as_timestamp("ts") == tp);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_qs");
        });
}

TEST_CASE("db: stmt bind scalar types round-trip", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ss ("
                                "u64 BIGINT UNSIGNED, s SMALLINT, us SMALLINT UNSIGNED, u INT UNSIGNED, "
                                "d DOUBLE, f FLOAT, b TINYINT(1), dt DATE, dtm DATETIME(6), tm TIME(6), j JSON)");
            co_await sess.query("DELETE FROM __httplib_ss");

            db::datetime dtm { 2024, 6, 1, 12, 34, 56, 123456 };
            auto tm = db::time::from_duration(std::chrono::microseconds { 3723456789 });
            boost::json::value jv {
                { "k", "v" }
            };
            co_await sess
                .stmt("INSERT INTO __httplib_ss VALUES "
                      "(:u64, :s, :us, :u, :d, :f, :b, :dt, :dtm, :tm, :j)")
                .bind("u64", uint64_t { 42 })
                .bind("s", short { -7 })
                .bind("us", static_cast<unsigned short>(7))
                .bind("u", 123u)
                .bind("d", 2.25)
                .bind("f", 1.5f)
                .bind("b", true)
                .bind("dt", db::date { 2024, 6, 1 })
                .bind("dtm", dtm)
                .bind("tm", tm)
                .bind("j", jv)
                .execute();

            auto r = co_await sess.query("SELECT u64, s, us, u, d, f, b, dt, dtm, tm, j FROM __httplib_ss");
            REQUIRE(r.row_count() == 1);
            REQUIRE(*r[0].as_uint64("u64") == 42);
            REQUIRE(*r[0].as_int64("s") == -7);
            REQUIRE(*r[0].as_uint64("us") == 7);
            REQUIRE(*r[0].as_uint64("u") == 123);
            REQUIRE(*r[0].as_double("d") == 2.25);
            REQUIRE(*r[0].as_float("f") == 1.5f);
            REQUIRE(*r[0].as_bool("b") == true);
            REQUIRE(*r[0].as_date("dt") == db::date { 2024, 6, 1 });
            REQUIRE(*r[0].as_datetime("dtm") == dtm);
            REQUIRE(*r[0].as_time("tm") == tm);
            REQUIRE(*r[0].as_json("j") == jv);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ss");
        });
}

TEST_CASE("db: into() supports small integer types", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ints (a INT, b INT, c INT, d INT)");
            co_await sess.query("DELETE FROM __httplib_ints");
            co_await sess.query("INSERT INTO __httplib_ints VALUES (1, 2, 3, 4)");

            std::optional<int> si;
            std::optional<short> ss;
            std::optional<unsigned> ui;
            std::optional<unsigned short> us;
            co_await sess.stmt("SELECT a, b, c, d FROM __httplib_ints")
                .execute(db::into(si, 0), db::into(ss, 1), db::into(ui, 2), db::into(us, 3));
            REQUIRE(si == 1);
            REQUIRE(ss == 2);
            REQUIRE(ui == 3u);
            REQUIRE(us == 4);

            std::vector<int> vi;
            std::vector<unsigned> vu;
            co_await sess.query("INSERT INTO __httplib_ints VALUES (5, 6, 7, 8)");
            co_await sess.stmt("SELECT a, c FROM __httplib_ints ORDER BY a").execute(db::into(vi, 0), db::into(vu, 1));
            REQUIRE(vi == std::vector<int> { 1, 5 });
            REQUIRE(vu == std::vector<unsigned> { 3u, 7u });

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ints");
        });
}

TEST_CASE("db: statement reuse (caching)", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_cache (id INT)");
            co_await sess.query("DELETE FROM __httplib_cache");
            co_await sess.query("INSERT INTO __httplib_cache VALUES (1),(2)");

            auto stmt = sess.stmt("SELECT id FROM __httplib_cache WHERE id = :id");
            std::optional<int64_t> v1;
            co_await stmt.bind(1).execute(db::into(v1, 0));
            REQUIRE(v1 == 1);

            std::optional<int64_t> v2;
            co_await stmt.bind(2).execute(db::into(v2, 0));
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
            db::mysql_config cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.max_cached_statements = 2;
            auto sess = co_await db::session::connect(ioc.get_executor(), cfg);
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_sc (id INT)");
            co_await sess.query("DELETE FROM __httplib_sc");
            co_await sess.query("INSERT INTO __httplib_sc VALUES (1),(2),(3),(4),(5),(6)");

            for (int round = 0; round < 3; ++round)
            {
                std::optional<int64_t> a, b, c;
                co_await sess.stmt("SELECT id FROM __httplib_sc WHERE id = :id").bind(1).execute(db::into(a, 0));
                co_await sess.stmt("SELECT id FROM __httplib_sc WHERE id > :id").bind(3).execute(db::into(b, 0));
                co_await sess.stmt("SELECT id FROM __httplib_sc WHERE id >= :id").bind(5).execute(db::into(c, 0));
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
                    .execute(db::into(id, "id"), db::into(name, "name"));
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
        [](db::connection_pool& pool) -> net::awaitable<void>
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
        [](db::connection_pool& pool) -> net::awaitable<void>
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
            db::mysql_config cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.charset = "latin1";
            auto sess = co_await db::session::connect(ioc.get_executor(), cfg);
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

TEST_CASE("db: async_acquire honors wait_timeout", "[db][integration]")
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
            db::connection_pool pool = make_pool(ioc.get_executor(), cfg);
            pool.start();
            auto h1 = co_await pool.async_acquire(); // holds the only connection

            auto t0 = std::chrono::steady_clock::now();
            REQUIRE_THROWS_AS(co_await pool.async_acquire(std::chrono::seconds(1)), std::runtime_error);
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
            db::connection_pool pool = make_pool(ioc.get_executor(), cfg);
            pool.start();

            {
                auto h1 = co_await pool.async_acquire();
                auto r = co_await h1->query("SELECT CONNECTION_ID() AS id");
                auto conn_id = *r[0].as_uint64("id");

                db::mysql_config kcfg;
                kcfg.user = "root";
                kcfg.password = "123456";
                auto killer = co_await db::session::connect(ioc.get_executor(), kcfg);
                co_await killer.query("KILL " + std::to_string(conn_id));

                REQUIRE_THROWS_AS(co_await h1->query("SELECT 1"), db::db_exception);
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
            db::connection_pool pool = make_pool(ioc.get_executor(), cfg);
            pool.start();

            {
                auto h1 = co_await pool.async_acquire();
                auto r = co_await h1->query("SELECT CONNECTION_ID() AS id");
                auto conn_id = *r[0].as_uint64("id");

                // kill h1 的连接，但不再执行查询（live 仍为 true）
                db::mysql_config kcfg;
                kcfg.user = "root";
                kcfg.password = "123456";
                auto killer = co_await db::session::connect(ioc.get_executor(), kcfg);
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
            db::mysql_config cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.time_zone = "+08:00";
            auto sess = co_await db::session::connect(ioc.get_executor(), cfg);
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ts (t TIMESTAMP)");
            co_await sess.query("DELETE FROM __httplib_ts");
            co_await sess.query("INSERT INTO __httplib_ts VALUES ('2024-01-01 12:00:00')");
            auto r = co_await sess.query("SELECT t FROM __httplib_ts");

            REQUIRE(r.column_type(0) == db::column_type::timestamp);
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

TEST_CASE("db: timestamp utc explicit +00:00", "[db][integration]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::mysql_config cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.time_zone = "+00:00"; // 显式 UTC 会话时区（默认空表示沿用服务器时区）
            auto sess = co_await db::session::connect(ioc.get_executor(), cfg);
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
        [](db::session& sess) -> net::awaitable<void>
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
            db::mysql_config cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.time_zone = "+08:00";
            auto sess = co_await db::session::connect(ioc.get_executor(), cfg);
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
            db::mysql_config cfg;
            cfg.user = "root";
            cfg.password = "123456";
            cfg.time_zone = "+08:00";
            auto sess = co_await db::session::connect(ioc.get_executor(), cfg);
            co_await sess.query("CREATE DATABASE IF NOT EXISTS test");
            co_await sess.query("USE test");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_tzn (d DATETIME, dt DATE, t TIME)");
            co_await sess.query("DELETE FROM __httplib_tzn");
            co_await sess.query("INSERT INTO __httplib_tzn VALUES ('2024-01-15 12:34:56', '2024-01-15', '12:34:56')");
            auto r = co_await sess.query("SELECT d, dt, t FROM __httplib_tzn");

            REQUIRE(r.column_type(0) == db::column_type::datetime);
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
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ext (id INT)");
            co_await sess.query("DELETE FROM __httplib_ext");
            co_await sess.query("INSERT INTO __httplib_ext VALUES (1),(2)");

            auto stmt = sess.stmt("SELECT id FROM __httplib_ext WHERE id = :id");
            std::optional<int64_t> v1;
            co_await stmt.bind(1).execute(db::into(v1, 0));
            REQUIRE(v1 == 1);

            std::optional<int64_t> v2;
            co_await stmt.bind(2).execute(db::into(v2, 0));
            REQUIRE(v1 == 1); // 重新 into 替换旧 extractor，v1 不再被写
            REQUIRE(v2 == 2);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ext");
        });
}

TEST_CASE("db: into re-declared per execute", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ip (v INT)");
            co_await sess.query("DELETE FROM __httplib_ip");
            co_await sess.query("INSERT INTO __httplib_ip VALUES (10),(20)");

            // 每次执行前声明输出（一次性，不持久）
            auto stmt = sess.stmt("SELECT v FROM __httplib_ip WHERE v = :v");
            std::optional<int64_t> out;
            co_await stmt.bind(10).execute(db::into(out, "v"));
            REQUIRE(out == 10);
            co_await stmt.bind(20).execute(db::into(out, "v"));
            REQUIRE(out == 20);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ip");
        });
}

TEST_CASE("db: into vector", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_vec (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_vec");
            co_await sess.query("INSERT INTO __httplib_vec VALUES (1, 'a'),(2, 'b'),(3, 'c')");

            // 按列下标
            std::vector<int64_t> ids;
            co_await sess.stmt("SELECT id FROM __httplib_vec ORDER BY id").execute(db::into(ids, 0));
            REQUIRE(ids == std::vector<int64_t> { 1, 2, 3 });

            // 按列名
            std::vector<std::string> names;
            co_await sess.stmt("SELECT name FROM __httplib_vec ORDER BY id").execute(db::into(names, "name"));
            REQUIRE(names == std::vector<std::string> { "a", "b", "c" });

            // 含 NULL 行时抛异常
            co_await sess.query("INSERT INTO __httplib_vec VALUES (NULL, NULL)");
            std::vector<int64_t> all;
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT id FROM __httplib_vec").execute(db::into(all, "id")),
                              std::runtime_error);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_vec");
        });
}

TEST_CASE("db: into positional column", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ipc (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_ipc");
            co_await sess.query("INSERT INTO __httplib_ipc VALUES (1, 'a'),(2, 'b')");

            // optional：第 N 次 into 对应第 N 列（SOCI 风格，不写列名/列号）
            std::optional<int64_t> id;
            std::optional<std::string> name;
            co_await sess.stmt("SELECT id, name FROM __httplib_ipc WHERE id = :id")
                .bind("id", 1)
                .execute(db::into(id), db::into(name));
            REQUIRE(id == 1);
            REQUIRE(name == "a");

            // vector：第 N 次 into 对应第 N 列
            std::vector<int64_t> ids;
            std::vector<std::string> names;
            co_await sess.stmt("SELECT id, name FROM __httplib_ipc ORDER BY id")
                .execute(db::into(ids), db::into(names));
            REQUIRE(ids == std::vector<int64_t> { 1, 2 });
            REQUIRE(names == std::vector<std::string> { "a", "b" });

            // 重新 into 时计数器从 0 重新计
            std::optional<int64_t> v;
            auto stmt = sess.stmt("SELECT id FROM __httplib_ipc WHERE id = :id");
            co_await stmt.bind("id", 1).execute(db::into(v));
            REQUIRE(v == 1);
            co_await stmt.bind("id", 2).execute(db::into(v));
            REQUIRE(v == 2);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ipc");
        });
}

TEST_CASE("db: bind arity mismatch throws", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_arity (a INT, b INT)");
            co_await sess.query("DELETE FROM __httplib_arity");

            REQUIRE_THROWS_AS(co_await sess.stmt("INSERT INTO __httplib_arity VALUES (:a, :b)").bind(1).execute(),
                              db::db_exception);
            REQUIRE_THROWS_AS(co_await sess.stmt("INSERT INTO __httplib_arity VALUES (:a)").bind(1).bind(2).execute(),
                              db::db_exception);

            // 连接仍可用（参数数量错误是客户端判定，不应废掉连接）
            auto r = co_await sess.query("SELECT 1 AS v");
            REQUIRE(*r[0].as_int64("v") == 1);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_arity");
        });
}

TEST_CASE("db: error message includes params", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_ep (id INT PRIMARY KEY)");
            co_await sess.query("DELETE FROM __httplib_ep");
            co_await sess.query("INSERT INTO __httplib_ep VALUES (1)");

            try
            {
                co_await sess.stmt("INSERT INTO __httplib_ep VALUES (:id)").bind(1).execute();
                REQUIRE(false);
            }
            catch (db::db_exception const& ex)
            {
                REQUIRE(std::string(ex.what()).find("params: [1]") != std::string::npos);
            }

            co_await sess.query("DROP TABLE IF EXISTS __httplib_ep");
        });
}

TEST_CASE("db: error message includes named params", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_enp (id INT PRIMARY KEY)");
            co_await sess.query("DELETE FROM __httplib_enp");
            co_await sess.query("INSERT INTO __httplib_enp VALUES (1)");

            try
            {
                co_await sess.stmt("INSERT INTO __httplib_enp VALUES (:id)").bind("id", 1).execute();
                REQUIRE(false);
            }
            catch (db::db_exception const& ex)
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
        {
            // 完全未绑定
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :n IS NULL AS r").execute(), std::runtime_error);

            // 绑了一部分，剩一个没绑
            REQUIRE_THROWS_AS(co_await sess.stmt("SELECT :a + :b AS x").bind("a", 1).execute(), std::runtime_error);

            // 全部绑定正常执行
            auto r = co_await sess.stmt("SELECT :a + :b AS x").bind("a", 1).bind("b", 2).execute();
            REQUIRE(*r[0].as_int64("x") == 3);
        });
}

TEST_CASE("db: named param reused multiple times", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
        {
            // :name 占位符按出现顺序做位置绑定（SOCI 风格）
            auto r = co_await sess.stmt("SELECT :a + :b AS x").bind(1).bind(2).execute();
            REQUIRE(*r[0].as_int64("x") == 3);
        });
}

TEST_CASE("db: named param ignores # comment", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            // MySQL 的 # 到行尾都是注释；注释里的 :tmp 不应被当作命名参数
            auto r = co_await sess.stmt("SELECT :a AS x # 调试 :tmp").bind("a", 42).execute();
            REQUIRE(*r[0].as_int64("x") == 42);
        });
}

TEST_CASE("db: bind once execute multiple times", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_intoedge (id INT, name VARCHAR(100))");
            co_await sess.query("DELETE FROM __httplib_intoedge");
            co_await sess.query("INSERT INTO __httplib_intoedge VALUES (1, 'alice'), (NULL, NULL)");

            // 越界列下标
            {
                std::optional<int64_t> v;
                REQUIRE_THROWS_AS(co_await sess.stmt("SELECT id FROM __httplib_intoedge").execute(db::into(v, 999)),
                                  std::out_of_range);
            }
            // 列名不存在
            {
                std::optional<int64_t> v;
                REQUIRE_THROWS_AS(co_await sess.stmt("SELECT id FROM __httplib_intoedge").execute(db::into(v, "nope")),
                                  std::runtime_error);
            }
            // 类型不匹配
            {
                std::optional<int64_t> v;
                REQUIRE_THROWS_AS(
                    co_await sess.stmt("SELECT name FROM __httplib_intoedge WHERE id = 1").execute(db::into(v, "name")),
                    std::runtime_error);
            }
            // 空结果不写 optional
            {
                std::optional<int64_t> v = 7;
                co_await sess.stmt("SELECT id FROM __httplib_intoedge WHERE id = 999").execute(db::into(v, "id"));
                REQUIRE(v == 7);
            }
            // NULL 值 -> nullopt
            {
                std::optional<int64_t> v = 7;
                co_await sess.stmt("SELECT id FROM __httplib_intoedge WHERE name IS NULL").execute(db::into(v, "id"));
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
            db::connection_pool pool = make_pool(ioc.get_executor());
            pool.start();
            auto handle = co_await pool.async_acquire();

            db::mysql_config kcfg;
            kcfg.user = "root";
            kcfg.password = "123456";
            auto killer = co_await db::session::connect(ioc.get_executor(), kcfg);

            std::string msg;
            try
            {
                co_await handle->with_transaction(
                    [&](db::session& s) -> net::awaitable<void>
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

    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            db::connection_pool pool = make_pool(ioc.get_executor(), cfg);
            pool.start();

            auto h1 = co_await pool.async_acquire();
            auto r = co_await h1->query("SELECT CONNECTION_ID() AS id");
            auto conn_id = *r[0].as_uint64("id");

            std::optional<db::connection_pool::session_handle> h2;
            net::co_spawn(
                ioc.get_executor(),
                [&pool, &h2]() -> net::awaitable<void> { h2.emplace(co_await pool.async_acquire()); },
                [](std::exception_ptr) {});

            // let the waiting acquire register itself
            net::steady_timer settle(ioc.get_executor());
            settle.expires_after(std::chrono::milliseconds(200));
            co_await settle.async_wait(boost::asio::use_awaitable);

            {
                db::mysql_config kcfg;
                kcfg.user = "root";
                kcfg.password = "123456";
                auto killer = co_await db::session::connect(ioc.get_executor(), kcfg);
                co_await killer.query("KILL " + std::to_string(conn_id));
            }
            REQUIRE_THROWS_AS(co_await h1->query("SELECT 1"), db::db_exception);

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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_wtxn (id INT)");
            co_await sess.query("DELETE FROM __httplib_wtxn");

            co_await sess.with_transaction(
                [](db::session& s) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_wtxr (id INT)");
            co_await sess.query("DELETE FROM __httplib_wtxr");

            try
            {
                co_await sess.with_transaction(
                    [](db::session& s) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
        {
            auto ok = co_await sess.ping();
            REQUIRE(ok);
        });
}

TEST_CASE("db: query_logger", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            std::string logged;
            size_t rows = 0;
            sess.set_query_logger(
                [&](db::query_log_entry const& e)
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
            db::mysql_config cfg;
            cfg.user = "root";
            cfg.password = "123456";
            auto sess = co_await db::session::connect(ioc.get_executor(), cfg);
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
            db::mysql_config cfg;
            cfg.user = "root";
            cfg.password = "123456";
            auto sess = co_await db::session::connect(ioc.get_executor(), cfg);
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
        [](db::session& sess) -> net::awaitable<void>
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
    run([](db::session& sess) -> net::awaitable<void>
        { REQUIRE_THROWS_AS(co_await sess.query("BOGUS SYNTAX ERROR"), db::db_exception); });
}

TEST_CASE("db: row out of bounds", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            auto r = co_await sess.query("SELECT 1 AS v");
            REQUIRE(r.row_count() == 1);
            REQUIRE_NOTHROW(r[0]);
        });
}

TEST_CASE("db: next_resultset on single resultset", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
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
        [](db::session& sess) -> net::awaitable<void>
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

TEST_CASE("db_middleware: throws when not registered", "[db][middleware]")
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
                    REQUIRE_THROWS_AS(mw::fetch<mw::db_middleware>(req), std::exception);
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

    static_assert(db::date { 2024, 2, 29 }.is_valid());
    static_assert(!db::date { 2023, 2, 29 }.is_valid());
    static_assert(db::date { 2024, 2, 29 }.is_leap_year());
    static_assert(db::date { 2024, 2, 29 }.days_in_month() == 29);
    static_assert(db::date::from_sys_days(db::date { 2024, 1, 2 }.to_sys_days()) == db::date { 2024, 1, 2 });
    static_assert(db::time { 1, 2, 3, 456789 }.to_duration() == microseconds { 3723456789LL });
    static_assert(db::time { 1, 2, 3, 456789 }.total_microseconds() == 3723456789LL);
    static_assert(db::time::from_duration(microseconds { -1 }) == db::time { 0, 0, 0, 1, true });
    static_assert(db::datetime { 2024, 1, 15, 12, 34, 56 }.is_valid());

    db::date d { 2024, 2, 29 };
    REQUIRE(d.is_valid());
    REQUIRE(d.is_leap_year());
    REQUIRE(d.days_in_month() == 29);
    REQUIRE_FALSE(db::date { 2023, 2, 29 }.is_valid());
    REQUIRE(db::date { 2023, 2, 28 }.is_valid());
    REQUIRE_FALSE(db::date { 2024, 4, 31 }.is_valid());
    REQUIRE_FALSE(db::date { 0, 0, 0 }.is_valid());

    REQUIRE(d.to_string() == "2024-02-29");
    auto d2 = db::date::from_string("2024-02-29");
    REQUIRE(d2.has_value());
    REQUIRE(*d2 == d);
    REQUIRE_FALSE(db::date::from_string("2024-02-30").has_value());
    REQUIRE_FALSE(db::date::from_string("2024/02/29").has_value());

    auto sd = d.to_sys_days();
    REQUIRE(db::date::from_sys_days(sd) == d);
    REQUIRE((d + days { 1 }) == db::date { 2024, 3, 1 });
    REQUIRE((db::date { 2024, 3, 1 } - days { 1 }) == d);
    REQUIRE((db::date { 2024, 3, 1 } - d) == days { 1 });
    REQUIRE(db::date { 2024, 3, 1 } > d);

    db::time t { 1, 2, 3, 456789 };
    REQUIRE(t.is_valid());
    REQUIRE_FALSE(db::time { 0, 60, 0, 0 }.is_valid());
    REQUIRE(t.to_duration() == hours { 1 } + minutes { 2 } + seconds { 3 } + microseconds { 456789 });
    REQUIRE(t.total_microseconds() == 3723456789LL);
    REQUIRE(db::time::from_duration(t.to_duration()) == t);
    REQUIRE(t.to_string() == "01:02:03.456789");
    REQUIRE(db::time { 1, 2, 3, 0 }.to_string() == "01:02:03");
    auto t2 = db::time::from_string("01:02:03.456789");
    REQUIRE(t2.has_value());
    REQUIRE(*t2 == t);
    auto t3 = db::time::from_string("01:02:03.5");
    REQUIRE(t3.has_value());
    REQUIRE(t3->microsecond == 500000);
    REQUIRE_FALSE(db::time::from_string("01:61:00").has_value());

    auto tn = db::time::from_duration(microseconds { -3723456789 });
    REQUIRE(tn.negative);
    REQUIRE(tn.hour == 1);
    REQUIRE(tn.minute == 2);
    REQUIRE(tn.second == 3);
    REQUIRE(tn.microsecond == 456789);
    REQUIRE(tn.to_duration() == microseconds { -3723456789 });
    REQUIRE(tn.total_microseconds() == -3723456789LL);
    REQUIRE(tn.to_string() == "-01:02:03.456789");
    auto tn2 = db::time::from_string("-01:02:03.456789");
    REQUIRE(tn2.has_value());
    REQUIRE(*tn2 == tn);
    REQUIRE(tn < t);
    REQUIRE_FALSE(db::time::from_string("-01:61:00").has_value());

    db::datetime dt { 2024, 1, 15, 12, 34, 56, 123456 };
    REQUIRE(dt.is_valid());
    auto tp = dt.to_time_point();
    REQUIRE(db::datetime::from_time_point(tp) == dt);
    REQUIRE(dt.to_string() == "2024-01-15 12:34:56.123456");
    auto dt2 = db::datetime::from_string("2024-01-15 12:34:56.123456");
    REQUIRE(dt2.has_value());
    REQUIRE(*dt2 == dt);
    auto dt3 = db::datetime::from_string("2024-01-15 12:34:56");
    REQUIRE(dt3.has_value());
    REQUIRE(dt3->microsecond == 0);
    REQUIRE_FALSE(db::datetime::from_string("2024-01-15T12:34:56").has_value());
    REQUIRE((db::datetime { 2024, 1, 15, 12, 34, 57 } - dt) == microseconds { 876544 });
    REQUIRE(db::datetime { 2024, 1, 15, 12, 34, 57 } > dt);

    std::hash<db::date> hd;
    REQUIRE(hd(d) == hd(db::date { 2024, 2, 29 }));
    REQUIRE(hd(d) != hd(db::date { 2024, 3, 1 }));
    std::hash<db::datetime> hdt;
    REQUIRE(hdt(dt) == hdt(db::datetime { 2024, 1, 15, 12, 34, 56, 123456 }));

    std::hash<db::time> ht;
    db::time z0 { 0, 0, 0, 0, false };
    db::time z1 { 0, 0, 0, 0, true };
    REQUIRE(z0 == z1);
    REQUIRE(ht(z0) == ht(z1));
}

TEST_CASE("db: temporal & narrow error handling", "[db][unit]")
{
    using namespace std::chrono;

    // date 非法 → to_sys_days / 算术抛异常
    REQUIRE_THROWS_AS((db::date { 2024, 2, 30 }).to_sys_days(), std::runtime_error);
    REQUIRE_THROWS_AS((db::date { 2024, 13, 1 }).to_sys_days(), std::runtime_error);
    REQUIRE_THROWS_AS((db::date { 0, 0, 0 }).to_sys_days(), std::runtime_error);
    REQUIRE_THROWS_AS((db::date { 2024, 2, 30 }) + days { 1 }, std::runtime_error);
    REQUIRE_THROWS_AS((db::date { 2024, 2, 30 }) - (db::date { 2024, 2, 28 }), std::runtime_error);

    // date::from_string 各种非法
    REQUIRE_FALSE(db::date::from_string("").has_value());
    REQUIRE_FALSE(db::date::from_string("2024-02").has_value());
    REQUIRE_FALSE(db::date::from_string("2024-02-30").has_value());
    REQUIRE_FALSE(db::date::from_string("2024-13-01").has_value());
    REQUIRE_FALSE(db::date::from_string("2024-00-10").has_value());
    REQUIRE_FALSE(db::date::from_string("2024-01-00").has_value());
    REQUIRE_FALSE(db::date::from_string("abcd-ef-gh").has_value());
    REQUIRE_FALSE(db::date::from_string("20240229").has_value());
    REQUIRE_FALSE(db::date::from_string("2024/02/29").has_value());
    REQUIRE_FALSE(db::date::from_string("2024--02-29").has_value());

    // time::from_string 各种非法
    REQUIRE_FALSE(db::time::from_string("").has_value());
    REQUIRE_FALSE(db::time::from_string("12:34").has_value());
    REQUIRE_FALSE(db::time::from_string("12:60:00").has_value());
    REQUIRE_FALSE(db::time::from_string("12:34:60").has_value());
    REQUIRE_FALSE(db::time::from_string("12:34:56.1234567").has_value());
    REQUIRE_FALSE(db::time::from_string("1a:00:00").has_value());
    REQUIRE_FALSE(db::time::from_string("-").has_value());
    REQUIRE_FALSE(db::time::from_string("--01:02:03").has_value());

    // datetime 非法 → is_valid / to_time_point
    REQUIRE_FALSE(db::datetime { 2024, 2, 30, 12, 0, 0 }.is_valid());
    REQUIRE_FALSE(db::datetime { 2024, 1, 1, 24, 0, 0 }.is_valid());
    REQUIRE_FALSE(db::datetime { 2024, 1, 1, 12, 60, 0 }.is_valid());
    REQUIRE_FALSE(db::datetime { 2024, 1, 1, 12, 0, 0, 1000000 }.is_valid());
    REQUIRE_THROWS_AS((db::datetime { 2024, 2, 30, 12, 0, 0 }).to_time_point(), std::runtime_error);
    REQUIRE_THROWS_AS((db::datetime { 2024, 1, 1, 12, 60, 0 }).to_time_point(), std::runtime_error);
    REQUIRE_THROWS_AS((db::datetime { 2024, 1, 1, 12, 0, 60 }).to_time_point(), std::runtime_error);

    // datetime::from_string 各种非法
    REQUIRE_FALSE(db::datetime::from_string("").has_value());
    REQUIRE_FALSE(db::datetime::from_string("2024-01-15").has_value());
    REQUIRE_FALSE(db::datetime::from_string("2024-01-15T12:34:56").has_value());
    REQUIRE_FALSE(db::datetime::from_string("2024-02-30 12:00:00").has_value());
    REQUIRE_FALSE(db::datetime::from_string("2024-01-15 12:60:00").has_value());
    REQUIRE_FALSE(db::datetime::from_string("2024-01-15 25:00:00").has_value());
    REQUIRE_FALSE(db::datetime::from_string("2024-01-15 -12:00:00").has_value());

    // narrow_int / narrow_uint 越界
    REQUIRE(db::detail::narrow_int<int>(std::optional<int64_t> { 12345 }) == 12345);
    REQUIRE_FALSE(db::detail::narrow_int<int>(std::nullopt).has_value());
    REQUIRE_THROWS_AS(db::detail::narrow_int<int>(std::optional<int64_t> { INT64_MAX }), std::runtime_error);
    REQUIRE_THROWS_AS(db::detail::narrow_int<int>(std::optional<int64_t> { INT64_MIN }), std::runtime_error);
    REQUIRE_THROWS_AS(db::detail::narrow_int<short>(std::optional<int64_t> { 70000 }), std::runtime_error);
    REQUIRE_THROWS_AS(db::detail::narrow_uint<unsigned short>(std::optional<uint64_t> { 70000 }), std::runtime_error);
    REQUIRE_THROWS_AS(db::detail::narrow_uint<unsigned>(std::optional<uint64_t> { UINT64_MAX }), std::runtime_error);
    REQUIRE(db::detail::narrow_uint<unsigned>(std::optional<uint64_t> { 4000000000ull }) == 4000000000u);
}

TEST_CASE("db: aggregate sum accuracy", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("DROP TABLE IF EXISTS __httplib_agg");
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_agg "
                                "(id INT PRIMARY KEY, n BIGINT, d DECIMAL(20, 2))");
            co_await sess.query("DELETE FROM __httplib_agg");
            co_await sess.query("INSERT INTO __httplib_agg VALUES (1, 10, 1.50), (2, 20, 2.25), (3, 30, 3.75)");

            // SUM(int) → DECIMAL（无小数），应可读为 int64
            auto r1 = co_await sess.query("SELECT SUM(n) AS s FROM __httplib_agg");
            REQUIRE(r1.row_count() == 1);
            REQUIRE(*r1[0].as_int64("s") == 60);
            REQUIRE(*r1[0].as_double("s") == 60.0);

            // 大数 SUM（两行 2^40 ± 100，加上前 3 行共 60）
            co_await sess.query("INSERT INTO __httplib_agg (id, n) VALUES (4, 1099511627776), (5, 1099511627876)");
            auto r2 = co_await sess.query("SELECT SUM(n) AS s FROM __httplib_agg");
            REQUIRE(*r2[0].as_int64("s") == 2199023255712LL);

            // SUM(decimal) → DECIMAL（带小数）
            auto r3 = co_await sess.query("SELECT SUM(d) AS s FROM __httplib_agg WHERE id <= 3");
            REQUIRE(*r3[0].as_double("s") == 7.50);
            REQUIRE(r3.column_type(r3.column_index("s")) == db::column_type::double_);

            // 多行聚合 + 分组
            auto r4 = co_await sess.query("SELECT SUM(n) AS s, COUNT(*) AS c FROM __httplib_agg");
            REQUIRE(*r4[0].as_int64("s") == 2199023255712LL);
            REQUIRE(*r4[0].as_int64("c") == 5);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_agg");
        });
}

TEST_CASE("db: sum small values", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("DROP TABLE IF EXISTS __httplib_small");
            co_await sess.query("CREATE TABLE __httplib_small (id INT PRIMARY KEY, v INT)");
            auto stmt = sess.stmt("INSERT INTO __httplib_small VALUES (:id, :v)");
            for (int i = 1; i <= 100; ++i)
            {
                co_await stmt.bind("id", i).bind("v", i).execute();
            }

            auto r1 = co_await sess.query("SELECT SUM(v) AS s FROM __httplib_small");
            REQUIRE(r1.row_count() == 1);
            REQUIRE(*r1[0].as_int64("s") == 5050);
            REQUIRE(*r1[0].as_double("s") == 5050.0);

            auto r2 = co_await sess.query("SELECT id % 2 AS g, SUM(v) AS s FROM __httplib_small GROUP BY g ORDER BY g");
            REQUIRE(r2.row_count() == 2);
            REQUIRE(*r2[0].as_int64("g") == 0);
            REQUIRE(*r2[0].as_int64("s") == 2550); // 偶数和
            REQUIRE(*r2[1].as_int64("g") == 1);
            REQUIRE(*r2[1].as_int64("s") == 2500); // 奇数和

            auto r3 = co_await sess.query("SELECT AVG(v) AS a FROM __httplib_small");
            REQUIRE(*r3[0].as_double("a") == 50.5);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_small");
        });
}

TEST_CASE("db: multi-row fetch accuracy", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query(
                "CREATE TABLE IF NOT EXISTS __httplib_multi "
                "(id INT PRIMARY KEY, name VARCHAR(20), score DOUBLE, n BIGINT, d DATE, dt DATETIME, note TEXT)");
            co_await sess.query("DELETE FROM __httplib_multi");
            co_await sess.query("INSERT INTO __httplib_multi VALUES "
                                "(1,'alpha',1.5,10,'2024-01-01','2024-01-01 08:00:00','x'),"
                                "(2,'beta',2.5,20,'2024-02-02','2024-02-02 09:30:00',NULL),"
                                "(3,'gamma',3.5,30,'2024-03-03','2024-03-03 10:00:00','z')");

            auto r = co_await sess.query("SELECT id, name, score, n, d, dt, note FROM __httplib_multi ORDER BY id");
            REQUIRE(r.row_count() == 3);
            REQUIRE(r.column_count() == 7);

            REQUIRE(*r[0].as_int64("id") == 1);
            REQUIRE(*r[0].as_string("name") == "alpha");
            REQUIRE(*r[0].as_double("score") == 1.5);
            REQUIRE(*r[0].as_int64("n") == 10);
            REQUIRE(*r[0].as_date("d") == db::date { 2024, 1, 1 });
            REQUIRE(*r[0].as_datetime("dt") == db::datetime { 2024, 1, 1, 8, 0, 0, 0 });
            REQUIRE(*r[0].as_string("note") == "x");

            REQUIRE(*r[1].as_string("name") == "beta");
            REQUIRE(*r[1].as_double("score") == 2.5);
            REQUIRE(*r[1].as_date("d") == db::date { 2024, 2, 2 });
            REQUIRE(*r[1].as_datetime("dt") == db::datetime { 2024, 2, 2, 9, 30, 0, 0 });
            REQUIRE(r[1].is_null("note"));

            REQUIRE(*r[2].as_string("name") == "gamma");
            REQUIRE(*r[2].as_double("score") == 3.5);
            REQUIRE(*r[2].as_int64("n") == 30);
            REQUIRE(*r[2].as_string("note") == "z");

            // 迭代器遍历
            std::vector<std::string> expected = { "alpha", "beta", "gamma" };
            size_t i = 0;
            for (auto const& row : r)
            {
                REQUIRE(*row.as_int64("id") == static_cast<int64_t>(i + 1));
                REQUIRE(*row.as_string("name") == expected[i]);
                ++i;
            }
            REQUIRE(i == 3);

            // into(vector)
            std::vector<std::string> names;
            co_await sess.query("SELECT name FROM __httplib_multi ORDER BY id", db::into(names));
            REQUIRE(names == std::vector<std::string> { "alpha", "beta", "gamma" });

            co_await sess.query("DROP TABLE IF EXISTS __httplib_multi");
        });
}

TEST_CASE("db: multi-row fetch 1000 rows", "[db][integration]")
{
    run(
        [](db::session& sess) -> net::awaitable<void>
        {
            co_await sess.query("CREATE TABLE IF NOT EXISTS __httplib_multi2 (id INT PRIMARY KEY, v VARCHAR(20))");
            co_await sess.query("DELETE FROM __httplib_multi2");
            auto stmt = sess.stmt("INSERT INTO __httplib_multi2 VALUES (:id, :v)");
            for (int i = 0; i < 1000; ++i)
            {
                co_await stmt.bind("id", i).bind("v", "row-" + std::to_string(i)).execute();
            }

            auto r = co_await sess.query("SELECT id, v FROM __httplib_multi2 ORDER BY id");
            REQUIRE(r.row_count() == 1000);
            int64_t sum = 0;
            for (size_t i = 0; i < r.row_count(); ++i)
            {
                REQUIRE(*r[i].as_int64("id") == static_cast<int64_t>(i));
                REQUIRE(*r[i].as_string("v") == "row-" + std::to_string(i));
                sum += *r[i].as_int64("id");
            }
            REQUIRE(sum == 999 * 1000 / 2);

            co_await sess.query("DROP TABLE IF EXISTS __httplib_multi2");
        });
}

#else
#include <catch2/catch_test_macros.hpp>
TEST_CASE("db: skipped", "[db]") { SKIP("HTTPLIB_ENABLED_DATABASE not enabled"); }
#endif
