#include "common.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include <boost/beast/http/status.hpp>
#include <catch2/catch_test_macros.hpp>

namespace net = httplib::net;

namespace
{
    using test_common::as_string;
    using test_common::run;
    using test_common::setup_logger;

    void
    set_text(httplib::server::response& resp, std::string_view body, httplib::status st = httplib::status::ok)
    {
        resp.set_string_content(body, "text/plain"sv, st);
    }

    struct counting_aspect
    {
        int* copies = nullptr;
        std::string* log = nullptr;

        counting_aspect(int& c, std::string& l)
            : copies(&c)
            , log(&l)
        {
        }
        counting_aspect(counting_aspect const& other)
            : copies(other.copies)
            , log(other.log)
        {
            ++*copies;
        }
        counting_aspect& operator=(counting_aspect const&) = delete;

        bool
        before(httplib::server::request&, httplib::server::response&)
        {
            *log += "before;";
            return true;
        }
        bool
        after(httplib::server::request&, httplib::server::response&)
        {
            *log += "after;";
            return true;
        }
    };

} // namespace

struct logging_aspect
{
    std::string& log;

    bool
    before(httplib::server::request&, httplib::server::response&)
    {
        log += "before;";
        return true;
    }

    bool
    after(httplib::server::request&, httplib::server::response&)
    {
        log += "after;";
        return true;
    }
};

TEST_CASE("Aspect: before and after are called", "[aspect]")
{
    std::string log;

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/aspect",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "handler"); },
                logging_aspect { log });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/aspect"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(as_string(resp) == "handler");
            REQUIRE(log == "before;after;");
        });
}

TEST_CASE("Aspect: before returning false stops handler chain", "[aspect]")
{
    struct blocking_aspect
    {
        bool
        before(httplib::server::request&, httplib::server::response& resp)
        {
            resp.set_string_content("blocked"sv, "text/plain"sv, httplib::status::forbidden);
            return false;
        }
        bool
        after(httplib::server::request&, httplib::server::response&)
        {
            return true;
        }
    };

    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/blocked",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "should-not-reach"); },
                blocking_aspect {});
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/blocked"));
            REQUIRE(resp.result() == httplib::status::forbidden);
            REQUIRE(as_string(resp) == "blocked");
        });
}

TEST_CASE("Aspect: multiple aspects chain in order", "[aspect]")
{
    std::string order;

    struct aspect_a
    {
        std::string& o;
        bool
        before(httplib::server::request&, httplib::server::response&)
        {
            o += "A.before;";
            return true;
        }
        bool
        after(httplib::server::request&, httplib::server::response&)
        {
            o += "A.after;";
            return true;
        }
    };

    struct aspect_b
    {
        std::string& o;
        bool
        before(httplib::server::request&, httplib::server::response&)
        {
            o += "B.before;";
            return true;
        }
        bool
        after(httplib::server::request&, httplib::server::response&)
        {
            o += "B.after;";
            return true;
        }
    };

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/multi-aspect",
                [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "ok"); },
                aspect_a { order },
                aspect_b { order });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/multi-aspect"));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(order == "A.before;B.before;A.after;B.after;");
        });
}

TEST_CASE("Aspect: lvalue is copied into the route at registration", "[aspect]")
{
    net::io_context ioc;
    httplib::server::http_server server(ioc.get_executor());

    int copies = 0;
    std::string log;
    {
        counting_aspect asp(copies, log);
        server.router().template set_http_handler<httplib::method::get>(
            "/owned",
            [](httplib::server::request&, httplib::server::response&) {},
            asp);

        // 注册即拥有：路由闭包必须持有副本，而不是调用方对象的引用。
        CHECK(copies >= 1);
    }
    // 调用方对象已析构；引用语义下闭包此刻已悬垂。
    CHECK(copies >= 1);
}

TEST_CASE("Aspect: lvalue aspect survives the registration scope", "[aspect]")
{
    int copies = 0;
    bool owns_copy = false;
    std::string log;

    run(
        [&](auto& server)
        {
            {
                counting_aspect asp(copies, log);
                server.router().template set_http_handler<httplib::method::get>(
                    "/owned-lifetime",
                    [](httplib::server::request&, httplib::server::response& resp) { set_text(resp, "handler"); },
                    asp);
            }
            owns_copy = copies > 0;
        },
        [&](auto& client) -> net::awaitable<void>
        {
            // 注册用的 lvalue 已析构；只有注册时拷贝过，路由才能安全使用。
            CHECK(owns_copy);
            if (!owns_copy)
            {
                // 未修复：跳过真实请求，避免对悬垂引用发起调用（UB）。
                co_return;
            }

            auto resp = UNWRAP(co_await client.async_get("/owned-lifetime"));
            CHECK(resp.result() == httplib::status::ok);
            CHECK(as_string(resp) == "handler");
            CHECK(log == "before;after;");
        });
}

TEST_CASE("Aspect: 404 handler also supports aspects", "[aspect]")
{
    std::string log;

    run(
        [&](auto& server)
        {
            server.router().set_http_not_found_handler([](httplib::server::request&, httplib::server::response& resp)
                                                       { set_text(resp, "custom-404", httplib::status::not_found); },
                                                       logging_aspect { log });
        },
        [&](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/non-existent"));
            REQUIRE(resp.result() == httplib::status::not_found);
            REQUIRE(as_string(resp) == "custom-404");
            REQUIRE(log == "before;after;");
        });
}
