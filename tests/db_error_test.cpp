#include "httplib/config.hpp"
#include "db/backend.hpp"
#include "db/render.hpp"
#include "httplib/db/binder.hpp"
#include "httplib/db/connection_pool.hpp"
#include "httplib/db/error.hpp"
#include "httplib/db/exception.hpp"
#include "httplib/db/result.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace db = httplib::db;
namespace net = httplib::net;

namespace
{
    namespace detail = db::detail;

    /// 运行 fn 并返回其抛出的 db_exception 的 code；未抛则返回空 code，让断言失败。
    template <typename Fn>
    boost::system::error_code
    code_thrown_by(Fn&& fn)
    {
        try
        {
            fn();
        }
        catch (db::db_exception const& e)
        {
            return e.code();
        }
        return {};
    }

    /// render_query 只借后端取占位符文本，最小实现即可（同 db_regression_test）。
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

    /// 每个枚举值都必须单独列出：漏一个这里就少一条覆盖。
    constexpr db::error kAllErrors[] = {
        db::error::invalid_parameters, db::error::column_not_found, db::error::type_mismatch,
        db::error::null_value,         db::error::not_initialized,  db::error::acquire_timeout,
        db::error::unknown_backend,
    };

    /// 单列一行的 string 列结果，供列访问 / 类型转换用例复用。
    db::result one_string_result()
    {
        return db::result(
            { db::result::resultset { { { db::field { std::string("hello") } } }, { "v" }, { db::column_type::string } } });
    }
} // namespace

TEST_CASE("db error: category name is stable", "[db][error]")
{
    auto ec = db::make_error_code(db::error::invalid_parameters);
    REQUIRE(std::string(ec.category().name()) == "httplib.db");
}

TEST_CASE("db error: every enumerator has a distinct, non-fallback message", "[db][error]")
{
    for (auto e : kAllErrors)
    {
        auto ec = db::make_error_code(e);
        // value 必须与枚举值一致，否则 message() 的 switch 会落到 "unknown error"。
        REQUIRE(ec.value() == static_cast<int>(e));
        auto msg = ec.message();
        REQUIRE_FALSE(msg.empty());
        REQUIRE(msg != "unknown error");
    }

    // 不同枚举值必须有不同消息：防止新增码时复制粘贴漏改，导致两条码含义混淆。
    for (std::size_t i = 0; i < std::size(kAllErrors); ++i)
    {
        for (std::size_t j = i + 1; j < std::size(kAllErrors); ++j)
        {
            REQUIRE(db::make_error_code(kAllErrors[i]).message() != db::make_error_code(kAllErrors[j]).message());
        }
    }
}

TEST_CASE("db error: enumerator implicitly converts to error_code", "[db][error]")
{
    // 依赖 is_error_code_enum<httplib::db::error> 特化；没有它这行无法编译。
    boost::system::error_code ec = db::error::column_not_found;
    REQUIRE(ec == db::make_error_code(db::error::column_not_found));
    REQUIRE(ec.value() == static_cast<int>(db::error::column_not_found));
    REQUIRE(ec != db::error::type_mismatch);
}

TEST_CASE("db error: db_exception carries the module code", "[db][error]")
{
    db::db_exception ex(db::make_error_code(db::error::null_value), "boom");
    REQUIRE(ex.code() == db::make_error_code(db::error::null_value));
    REQUIRE_FALSE(ex.code() == db::make_error_code(db::error::type_mismatch));
}

TEST_CASE("db error: render parameter mismatch maps to invalid_parameters", "[db][error]")
{
    stub_backend def;

    // SQL 有 :a 占位符但未提供任何绑定 → 命名参数未绑定。
    REQUIRE(code_thrown_by([&] { (void)detail::render_query("SELECT :a AS x", {}, def); })
            == db::make_error_code(db::error::invalid_parameters));

    // 位置绑定与命名绑定混用 → 禁止。
    REQUIRE(code_thrown_by([&] { (void)detail::render_query("SELECT :a AS x", { db::bind(1), db::bind("a", 2) }, def); })
            == db::make_error_code(db::error::invalid_parameters));
}

TEST_CASE("db error: unknown column maps to column_not_found", "[db][error]")
{
    auto r = one_string_result();

    REQUIRE(code_thrown_by([&] { (void)r.column_index("nope"); })
            == db::make_error_code(db::error::column_not_found));
}

TEST_CASE("db error: failed field conversion maps to type_mismatch", "[db][error]")
{
    auto r = one_string_result();

    // 字符串列按 int64 取 → 类型不符。
    REQUIRE(code_thrown_by([&] { (void)r[0].as_int64(0); }) == db::make_error_code(db::error::type_mismatch));
}

TEST_CASE("db error: unknown backend maps to unknown_backend", "[db][error]")
{
    net::io_context ioc;
    REQUIRE(code_thrown_by([&] { (void)db::make_pool(ioc.get_executor(), "no_such_backend", ""); })
            == db::make_error_code(db::error::unknown_backend));
}
