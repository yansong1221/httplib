#ifdef HTTPLIB_ENABLED_DATABASE
#    include "db/stmt_cache.h"
#    include "httplib/config.hpp"
#    include "httplib/db/db_config.hpp"
#    include "httplib/db/db_connection.hpp"
#    include "httplib/db/db_connection_pool.hpp"
#    include "httplib/db/db_result.hpp"
#    include "httplib/db/mysql_connection.hpp"
#    include <boost/asio/co_spawn.hpp>
#    include <boost/asio/thread_pool.hpp>
#    include <boost/asio/use_future.hpp>
#    include <boost/mysql/statement.hpp>
#    include <catch2/catch_test_macros.hpp>

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

TEST_CASE("stmt_cache: empty cache", "[db]")
{
    db::stmt_cache cache(8);
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.max_size() == 8);
    REQUIRE(cache.find("SELECT 1") == nullptr);
}

TEST_CASE("stmt_cache: insert and find", "[db]")
{
    db::stmt_cache cache(8);
    boost::mysql::statement stmt;
    auto evicted = cache.insert("SELECT 1", stmt);
    REQUIRE_FALSE(evicted.has_value());
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.find("SELECT 1") != nullptr);
}

TEST_CASE("stmt_cache: LRU eviction", "[db]")
{
    db::stmt_cache cache(3);
    boost::mysql::statement stmt;
    cache.insert("A", stmt);
    cache.insert("B", stmt);
    cache.insert("C", stmt);
    REQUIRE(cache.size() == 3);
    auto evicted = cache.insert("D", stmt);
    REQUIRE(evicted.has_value());
    REQUIRE(cache.find("A") == nullptr);
    REQUIRE(cache.find("D") != nullptr);
}

TEST_CASE("stmt_cache: find promotes to MRU", "[db]")
{
    db::stmt_cache cache(3);
    boost::mysql::statement stmt;
    cache.insert("A", stmt);
    cache.insert("B", stmt);
    cache.insert("C", stmt);
    cache.find("A");
    auto evicted = cache.insert("D", stmt);
    REQUIRE(evicted.has_value());
    REQUIRE(cache.find("B") == nullptr);
    REQUIRE(cache.find("A") != nullptr);
}

TEST_CASE("stmt_cache: disabled cache (max_size=0)", "[db]")
{
    db::stmt_cache cache(0);
    boost::mysql::statement stmt;
    REQUIRE(cache.insert("SQL", stmt).has_value());
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.find("SQL") == nullptr);
}

TEST_CASE("stmt_cache: erase and clear", "[db]")
{
    db::stmt_cache cache(8);
    boost::mysql::statement stmt;
    cache.insert("SQL", stmt);
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.erase("SQL").has_value());
    REQUIRE(cache.size() == 0);
    REQUIRE_FALSE(cache.erase("MISSING").has_value());

    cache.insert("A", stmt);
    cache.insert("B", stmt);
    REQUIRE(cache.size() == 2);
    auto all = cache.clear();
    REQUIRE(all.size() == 2);
    REQUIRE(cache.size() == 0);
}

TEST_CASE("db_result: basics", "[db]")
{
    db::db_result r;
    REQUIRE(r.empty());
    REQUIRE(r.size() == 0);
    REQUIRE(r.affected_rows == 0);
    REQUIRE(r.insert_id == 0);

    r.columns = { "id", "name" };
    REQUIRE(r.column_index("id") == 0);
    REQUIRE(r.column_index("name") == 1);
    REQUIRE(r.column_index("missing") == db::db_result::npos);
}

TEST_CASE("db_config: defaults", "[db]")
{
    db::db_config c;
    REQUIRE(c.host == "127.0.0.1");
    REQUIRE(c.port == 3306);
    REQUIRE(c.min_connections == 2);
    REQUIRE(c.max_connections == 16);
    REQUIRE(c.stmt_cache_size == 64);
}

// ===== Integration =====

TEST_CASE("mysql: connect and SELECT 1", "[db][integration]")
{
    net::thread_pool pool(1);
    auto ex = pool.get_executor();
    auto result = net::co_spawn(
                      ex,
                      [ex]() -> net::awaitable<std::string>
                      {
                          auto conn = co_await db::mysql_connection::create(ex, make_config());
                          REQUIRE(conn->is_alive());
                          auto rows = co_await conn->query("SELECT 1 AS val");
                          co_return rows[0][rows.column_index("val")];
                      },
                      net::use_future)
                      .get();
    REQUIRE(result == "1");
    pool.join();
}

TEST_CASE("mysql: parameterized query", "[db][integration]")
{
    net::thread_pool pool(1);
    auto ex = pool.get_executor();
    auto result = net::co_spawn(
                      ex,
                      [ex]() -> net::awaitable<std::string>
                      {
                          auto conn = co_await db::mysql_connection::create(ex, make_config());
                          std::vector<std::string> params = { "hello" };
                          auto rows = co_await conn->query("SELECT ? AS msg", params);
                          co_return rows[0][rows.column_index("msg")];
                      },
                      net::use_future)
                      .get();
    REQUIRE(result == "hello");
    pool.join();
}

TEST_CASE("mysql: ping", "[db][integration]")
{
    net::thread_pool pool(1);
    auto ex = pool.get_executor();
    auto alive = net::co_spawn(
                     ex,
                     [ex]() -> net::awaitable<bool>
                     {
                         auto conn = co_await db::mysql_connection::create(ex, make_config());
                         co_return co_await conn->ping();
                     },
                     net::use_future)
                     .get();
    REQUIRE(alive);
    pool.join();
}

TEST_CASE("mysql: CRUD", "[db][integration]")
{
    net::thread_pool pool(1);
    auto ex = pool.get_executor();
    auto [insert_id, name] = net::co_spawn(
                                 ex,
                                 [ex]() -> net::awaitable<std::pair<int, std::string>>
                                 {
                                     auto conn = co_await db::mysql_connection::create(ex, make_config());
                                     co_await conn->query("CREATE DATABASE IF NOT EXISTS test");
                                     co_await conn->query("USE test");

                                     co_await conn->query("CREATE TABLE IF NOT EXISTS __httplib_test "
                                                          "(id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(100))");

                                     std::vector<std::string> params = { "alice" };
                                     auto r
                                         = co_await conn->query("INSERT INTO __httplib_test (name) VALUES (?)", params);

                                     auto rows = co_await conn->query("SELECT * FROM __httplib_test");
                                     std::string n;
                                     if (!rows.empty() && rows.column_index("name") != db::db_result::npos)
                                     {
                                         n = rows[0][rows.column_index("name")];
                                     }

                                     co_await conn->query("DROP TABLE IF EXISTS __httplib_test");
                                     co_return std::pair((int)r.insert_id, n);
                                 },
                                 net::use_future)
                                 .get();
    REQUIRE(insert_id > 0);
    REQUIRE(name == "alice");
    pool.join();
}

TEST_CASE("mysql: transaction commit", "[db][integration]")
{
    net::thread_pool pool(1);
    auto ex = pool.get_executor();
    auto [ok, id_val]
        = net::co_spawn(
              ex,
              [ex]() -> net::awaitable<std::pair<bool, int>>
              {
                  auto conn = co_await db::mysql_connection::create(ex, make_config());
                  co_await conn->query("CREATE DATABASE IF NOT EXISTS test");
                  co_await conn->query("USE test");
                  co_await conn->query("CREATE TABLE IF NOT EXISTS __httplib_tx_test (id INT PRIMARY KEY)");

                  co_await conn->begin_transaction();
                  REQUIRE(conn->in_transaction());

                  std::vector<std::string> params = { "42" };
                  co_await conn->query("INSERT INTO __httplib_tx_test (id) VALUES (?)", params);
                  co_await conn->commit();
                  REQUIRE_FALSE(conn->in_transaction());

                  auto rows = co_await conn->query("SELECT * FROM __httplib_tx_test");
                  int v = 0;
                  if (!rows.empty() && rows.column_index("id") != db::db_result::npos)
                  {
                      v = std::stoi(rows[0][rows.column_index("id")]);
                  }

                  co_await conn->query("DROP TABLE IF EXISTS __httplib_tx_test");
                  co_return std::pair(true, v);
              },
              net::use_future)
              .get();
    REQUIRE(ok);
    REQUIRE(id_val == 42);
    pool.join();
}

TEST_CASE("mysql: transaction rollback", "[db][integration]")
{
    net::thread_pool pool(1);
    auto ex = pool.get_executor();
    auto ok = net::co_spawn(
                  ex,
                  [ex]() -> net::awaitable<bool>
                  {
                      auto conn = co_await db::mysql_connection::create(ex, make_config());
                      co_await conn->query("CREATE DATABASE IF NOT EXISTS test");
                      co_await conn->query("USE test");
                      co_await conn->query("CREATE TABLE IF NOT EXISTS __httplib_rb_test (val INT)");

                      co_await conn->begin_transaction();
                      REQUIRE(conn->in_transaction());

                      std::vector<std::string> params = { "99" };
                      co_await conn->query("INSERT INTO __httplib_rb_test (val) VALUES (?)", params);
                      co_await conn->rollback();
                      REQUIRE_FALSE(conn->in_transaction());

                      co_await conn->query("DROP TABLE IF EXISTS __httplib_rb_test");
                      co_return true;
                  },
                  net::use_future)
                  .get();
    REQUIRE(ok);
    pool.join();
}

TEST_CASE("pool: acquire, query, release", "[db][integration]")
{
    auto config = make_config();
    config.min_connections = 1;
    config.max_connections = 4;
    config.idle_check_interval = std::chrono::seconds(0);
    config.health_check_interval = std::chrono::seconds(0);

    net::thread_pool pool(1);
    auto ex = pool.get_executor();
    auto row_count
        = net::co_spawn(
              ex,
              [ex, cfg = config]() -> net::awaitable<int>
              {
                  auto pc = std::make_shared<db::db_connection_pool>(ex, cfg, db::mysql_connection::make_factory());
                  co_await pc->init();
                  REQUIRE(pc->idle_count() >= 1);

                  auto conn = co_await pc->acquire();
                  REQUIRE(conn->is_alive());
                  REQUIRE(pc->active_count() == 1);

                  auto rows = co_await conn->query("SELECT 1 AS n");
                  pc->release(std::move(conn));
                  REQUIRE(pc->active_count() == 0);

                  co_await pc->shutdown();
                  co_return (int) rows.size();
              },
              net::use_future)
              .get();
    REQUIRE(row_count == 1);
    pool.join();
}

// ===== Middleware =====

#    include "common.hpp"
#    include "httplib/server/middleware/db_middleware.hpp"
#    include "httplib/server/middleware/db_query_log.hpp"
#    include "httplib/server/request.hpp"
#    include "httplib/server/response.hpp"

TEST_CASE("db_middleware: inject connection into request", "[db][integration][middleware]")
{
    auto config = make_config();
    config.min_connections = 1;
    config.max_connections = 4;
    config.idle_check_interval = std::chrono::seconds(0);
    config.health_check_interval = std::chrono::seconds(0);

    net::thread_pool db_pool(2);
    auto ex = db_pool.get_executor();
    auto pool = std::make_shared<db::db_connection_pool>(ex, config, db::mysql_connection::make_factory());

    net::co_spawn(ex, [&]() -> net::awaitable<void> { co_await pool->init(); }, net::use_future).get();

    test_common::test_scaffold ts;
    ts.router().use(httplib::server::middleware::db_middleware(pool));

    ts.router().set_http_handler<http::verb::get>(
        "/db/echo",
        [](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto conn = httplib::server::middleware::get_db_connection(req);
            auto result = co_await conn->query("SELECT 'db_mw_ok' AS status");
            auto idx = result.column_index("status");
            resp.set_string_content(result[0][idx], "text/plain"sv);
        });

    ts.start();
    auto resp = UNWRAP(ts.client->get("/db/echo"));
    REQUIRE(resp.result() == http::status::ok);
    REQUIRE(test_common::as_string(resp) == "db_mw_ok");

    net::co_spawn(ex, [&]() -> net::awaitable<void> { co_await pool->shutdown(); }, net::use_future).get();
    db_pool.join();
}

TEST_CASE("db_middleware: throws when no middleware registered", "[db][middleware]")
{
    test_common::test_scaffold ts;
    ts.router().set_http_handler<http::verb::get>(
        "/db/nomw",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            REQUIRE_THROWS_AS(httplib::server::middleware::get_db_connection(req), std::runtime_error);
            resp.set_string_content("ok"sv, "text/plain"sv);
        });

    ts.start();
    auto resp = UNWRAP(ts.client->get("/db/nomw"));
    REQUIRE(resp.result() == http::status::ok);
}

TEST_CASE("db_query_log_middleware: logs queries", "[db][integration][middleware]")
{
    auto config = make_config();
    config.min_connections = 1;
    config.max_connections = 4;
    config.idle_check_interval = std::chrono::seconds(0);
    config.health_check_interval = std::chrono::seconds(0);

    test_common::test_scaffold ts;

    auto pool = std::make_shared<db::db_connection_pool>(ts.executor(), config, db::mysql_connection::make_factory());

    std::vector<httplib::server::middleware::query_log_entry> captured;

    httplib::server::middleware::query_log_options qlopts;
    qlopts.on_request_complete
        = [&](httplib::server::request const&, std::vector<httplib::server::middleware::query_log_entry> const& entries)
    { captured = entries; };

    net::co_spawn(
        ts.executor(),
        [&]() -> net::awaitable<void>
        {
            co_await pool->init();
            ts.router().use(httplib::server::middleware::db_middleware(pool));
            ts.router().use(httplib::server::middleware::db_query_log_middleware(qlopts));

            ts.router().set_http_handler<http::verb::get>(
                "/db/logged",
                [&](httplib::server::request& req, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto conn = httplib::server::middleware::get_db_connection(req);
                    co_await conn->query("SELECT 1 AS a");
                    co_await conn->query("SELECT 2 AS b");
                    resp.set_string_content("logged"sv, "text/plain"sv);
                });
            ts.start();

            auto resp = UNWRAP(co_await ts.client->async_get("/db/logged"));

            REQUIRE(resp.result() == http::status::ok);
            REQUIRE(captured.size() >= 2);

            co_await ts.server.async_stop();
            co_await pool->shutdown();
        },
        [](std::exception_ptr e)
        {
            if (e)
            {
                std::rethrow_exception(e);
            }
        });

    ts.join();
}

#else
#    include <catch2/catch_test_macros.hpp>
TEST_CASE("db: skipped", "[db]") { SKIP("HTTPLIB_ENABLED_DATABASE not enabled"); }
#endif
