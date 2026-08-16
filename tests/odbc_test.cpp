#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/db/binder.hpp"
#include "httplib/db/exception.hpp"
#include "httplib/db/session.hpp"
#include "httplib/db/temporal.hpp"
#include <boost/asio/io_context.hpp>
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
            "c DECIMAL(18,4) NULL, d VARBINARY(64) NULL, e DATE NULL, f DATETIME2(6) NULL, g TIME(6) NULL)");
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
            auto sess = co_await db::session::connect(ioc.get_executor(), odbc_test_config());
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

            // time_point 绑定（UTC 墙上时钟）
            auto tp = std::chrono::system_clock::time_point { std::chrono::milliseconds { 1709629230000 } };
            co_await sess.query("INSERT INTO httplib_odbc_t (f) VALUES (:f)", db::bind("f", tp));
            auto r5 = co_await sess.query(
                "SELECT f FROM httplib_odbc_t WHERE f >= '2000-01-01' AND f IS NOT NULL ORDER BY id DESC");
            auto expected = db::datetime::from_time_point(tp);
            REQUIRE(r5.row_count() >= 1);
            REQUIRE(*r5[0].as_datetime("f") == expected);

            // 数组展开（IN）
            auto r6 = co_await sess.query("SELECT COUNT(*) AS c FROM httplib_odbc_i WHERE v IN (:vs)",
                                          db::bind("vs", std::vector<int64_t> { 1, 2 }));
            REQUIRE(*r6[0].as_int64("c") == 0);

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

TEST_CASE("db(odbc): VALUES column-wise batch insert", "[db][odbc]")
{
    net::io_context ioc;
    std::exception_ptr err;
    net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            auto sess = co_await db::session::connect(ioc.get_executor(), odbc_test_config());
            co_await setup_database(sess);

            // 多列列式：每列一个数组，等长展开为多行
            auto ins = co_await sess.query(
                "INSERT INTO httplib_odbc_t (a, b) VALUES (:a, :b)",
                db::bind("a", std::vector<int64_t> { 1, 2, 3 }),
                db::bind("b", std::vector<std::string> { std::string("x"), std::string("y"), std::string("z") }));
            REQUIRE(ins.affected_rows() == 3);
            auto r = co_await sess.query("SELECT COUNT(*) AS c FROM httplib_odbc_t WHERE a IN (1, 2, 3)");
            REQUIRE(*r[0].as_int64("c") == 3);

            // 1000 行单列批量插入（SQL Server 单语句参数上限 2100，单列 1000 安全）
            std::vector<int64_t> ids;
            ids.reserve(1000);
            for (int64_t i = 0; i < 1000; ++i)
            {
                ids.push_back(i);
            }
            auto big = co_await sess.query("INSERT INTO httplib_odbc_i (v) VALUES (:v)", db::bind("v", ids));
            REQUIRE(big.affected_rows() == 1000);
            auto sum = co_await sess.query("SELECT COUNT(*) AS c, SUM(v) AS s FROM httplib_odbc_i");
            REQUIRE(*sum[0].as_int64("c") == 1000);
            REQUIRE(*sum[0].as_int64("s") == 499500);

            // 空数组 → 抛
            REQUIRE_THROWS_AS(co_await sess.query("INSERT INTO httplib_odbc_i (v) VALUES (:v)",
                                                  db::bind("v", std::vector<int64_t> {})),
                              std::runtime_error);
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
            auto sess = co_await db::session::connect(ioc.get_executor(), odbc_test_config());
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
            auto sess = co_await db::session::connect(ioc.get_executor(), odbc_test_config());
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
#endif // HTTPLIB_ENABLED_DATABASE