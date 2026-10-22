#include "common.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/error.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
    template <typename F>
    void
    run(F&& f)
    {
        net::io_context ioc;
        std::exception_ptr err;
        net::co_spawn(ioc, [&]() -> net::awaitable<void> { co_await f(ioc); }, [&](std::exception_ptr e) { err = e; });
        ioc.run();
        if (err)
        {
            std::rethrow_exception(err);
        }
    }

    /// 每个枚举值都必须单独列出：漏一个这里就少一条覆盖。
    constexpr httplib::client::error kAllErrors[] = {
        httplib::client::error::http_status,        httplib::client::error::too_many_redirects,
        httplib::client::error::protocol_violation, httplib::client::error::invalid_url,
        httplib::client::error::truncated_body,     httplib::client::error::cache_unavailable,
        httplib::client::error::acquire_timeout,    httplib::client::error::invalid_handle,
        httplib::client::error::not_initialized,    httplib::client::error::already_in_use,
    };
} // namespace

TEST_CASE("client error: category name is stable", "[client][error]")
{
    auto ec = httplib::client::make_error_code(httplib::client::error::http_status);
    REQUIRE(std::string(ec.category().name()) == "httplib.client");
}

TEST_CASE("client error: every enumerator has a distinct, non-fallback message", "[client][error]")
{
    for (auto e : kAllErrors)
    {
        auto ec = httplib::client::make_error_code(e);
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
            REQUIRE(httplib::client::make_error_code(kAllErrors[i]).message()
                    != httplib::client::make_error_code(kAllErrors[j]).message());
        }
    }
}

TEST_CASE("client error: enumerator implicitly converts to error_code", "[client][error]")
{
    // 依赖 is_error_code_enum<httplib::client::error> 特化；没有它这行无法编译。
    boost::system::error_code ec = httplib::client::error::invalid_url;
    REQUIRE(ec == httplib::client::make_error_code(httplib::client::error::invalid_url));
    REQUIRE(std::string(ec.category().name()) == "httplib.client");
    REQUIRE(ec != httplib::client::error::invalid_handle);
}

TEST_CASE("client error: pool acquire with a bad URL maps to invalid_url", "[client][error][client_pool]")
{
    // 回归：URL 解析失败必须归一成模块码，而不是把 url 模块的原始错误码漏给调用方。
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), { .max_size = 1 });

            auto h = co_await p.async_acquire("not a url at all");
            REQUIRE(h.has_error());
            REQUIRE(h.error() == httplib::client::make_error_code(httplib::client::error::invalid_url));

            p.stop();
            co_return;
        });
}

TEST_CASE("client error: URL constructor maps parse failure to invalid_url", "[client][error]")
{
    net::io_context ioc;

    try
    {
        httplib::client::http_client c(ioc, "not a url at all");
        FAIL("expected boost::system::system_error");
    }
    catch (boost::system::system_error const& e)
    {
        CHECK(e.code() == httplib::client::make_error_code(httplib::client::error::invalid_url));
    }

    httplib::client::http_client c(ioc, "http://127.0.0.1:8080");
    CHECK(c.host() == "127.0.0.1");
    CHECK(c.port() == 8080);
    CHECK(c.scheme() == httplib::url::scheme::plain);
}

TEST_CASE("client error: released handle reports invalid_handle, not a pool-capacity error",
          "[client][error][client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), { .max_size = 1 });

            auto h = co_await p.async_acquire("127.0.0.1", 80, httplib::url::scheme::plain);
            REQUIRE(h);

            // 主动归还后，句柄不持有连接，必须诚实报 invalid_handle。
            h.release();
            REQUIRE(h.has_error());
            REQUIRE(h.error() == httplib::client::make_error_code(httplib::client::error::invalid_handle));

            p.stop();
            co_return;
        });
}

TEST_CASE("client error: acquire builds the client from host/port/scheme, not a re-parsed URL",
          "[client][error][client_pool]")
{
    run(
        [](net::io_context& ioc) -> net::awaitable<void>
        {
            httplib::client::http_client_pool p(ioc.get_executor(), { .max_size = 1 });

            // host 含空格，不是合法的 URL 片段。池必须用 host/port/scheme 组件直接构造
            // client（校验延迟到 connect），而不是把 host 拼成 URL 再 parse 一遍——后者
            // 会在这里抛异常，把本应惰性暴露的问题变成借出失败。
            auto h = co_await p.async_acquire("bad host", 80, httplib::url::scheme::plain);
            REQUIRE(h);
            REQUIRE_FALSE(h.has_error());
            REQUIRE(h->host() == "bad host");

            h.release();
            p.stop();
            co_return;
        });
}

TEST_CASE("client error: moved-from handle reports invalid_handle", "[client][error][client_pool]")
{
    httplib::client::http_client_pool::client_handle empty;
    auto moved = std::move(empty);

    REQUIRE(moved.has_error());
    REQUIRE(moved.error() == httplib::client::make_error_code(httplib::client::error::invalid_handle));
    // 源对象被移动后同样是空句柄。
    REQUIRE(empty.has_error());
    REQUIRE(empty.error() == httplib::client::make_error_code(httplib::client::error::invalid_handle));
}
