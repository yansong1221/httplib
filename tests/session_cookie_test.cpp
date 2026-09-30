#include "common.hpp"
#include "httplib/server/middleware/data.hpp"
#include "httplib/server/middleware/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "server/middleware/memory_store.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace mw = httplib::server::middleware;
namespace net = httplib::net;

namespace
{
    using test_common::as_string;
    using test_common::run;
    using test_common::setup_logger;

} // namespace

// ===== session middleware tests =====

TEST_CASE("Session: middleware creates new session ID", "[session]")
{
    mw::session_middleware sm;

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/visit",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto sess = mw::fetch<mw::session_middleware>(req).value();
                    REQUIRE_FALSE(sess->id().empty());
                    resp.set_string_content("ok"sv, "text/plain"sv);
                },
                sm);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/visit"));
            REQUIRE(resp.result() == httplib::status::ok);

            auto set_cookie = std::string(resp[httplib::field::set_cookie]);
            REQUIRE_FALSE(set_cookie.empty());
            REQUIRE(set_cookie.starts_with("session_id="));
            co_return;
        });
}

TEST_CASE("Session: middleware persists data across requests", "[session]")
{
    mw::session_middleware sm;

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::post>(
                "/login",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto sess = mw::fetch<mw::session_middleware>(req).value();
                    sess->set("user", "alice");
                    resp.set_string_content("logged-in"sv, "text/plain"sv);
                },
                sm);

            server.router().template set_http_handler<httplib::method::get>(
                "/whoami",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto sess = mw::fetch<mw::session_middleware>(req).value();
                    auto user = sess->get("user");
                    resp.set_string_content(user.value_or("anonymous"), "text/plain"sv);
                },
                sm);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp1 = UNWRAP(co_await client.async_post("/login", std::string_view(""), "text/plain"sv));
            REQUIRE(resp1.result() == httplib::status::ok);
            auto cookie = std::string(resp1[httplib::field::set_cookie]);

            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::cookie, cookie);

            auto resp2 = UNWRAP(co_await client.async_get("/whoami", {}, hdrs));
            REQUIRE(resp2.result() == httplib::status::ok);
            REQUIRE(as_string(resp2) == "alice");
            co_return;
        });
}

TEST_CASE("Session: get_session returns valid pointer", "[session]")
{
    mw::session_middleware sm;

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/data",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto sess = mw::fetch<mw::session_middleware>(req).value();
                    REQUIRE(sess);
                    sess->set("count", "1");
                    auto c = sess->get("count");
                    REQUIRE(c.has_value());
                    REQUIRE(*c == "1");
                    resp.set_string_content("ok"sv, "text/plain"sv);
                },
                sm);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/data"));
            REQUIRE(resp.result() == httplib::status::ok);
            co_return;
        });
}

TEST_CASE("Session: session has and remove", "[session]")
{
    mw::session_middleware sm;

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ops",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto sess = mw::fetch<mw::session_middleware>(req).value();
                    sess->set("temp", "data");
                    REQUIRE(sess->has("temp"));
                    REQUIRE_FALSE(sess->empty());
                    sess->remove("temp");
                    REQUIRE_FALSE(sess->has("temp"));
                    REQUIRE(sess->empty());
                    resp.set_string_content("ok"sv, "text/plain"sv);
                },
                sm);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/ops"));
            REQUIRE(resp.result() == httplib::status::ok);
            co_return;
        });
}

TEST_CASE("Session: custom store can be injected", "[session]")
{
    auto store = std::make_shared<mw::memory_session_store>(std::chrono::seconds(60));
    mw::session_middleware sm(store);

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/custom",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto sess = mw::fetch<mw::session_middleware>(req).value();
                    sess->set("store", "injected");
                    resp.set_string_content("ok"sv, "text/plain"sv);
                },
                sm);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/custom"));
            REQUIRE(resp.result() == httplib::status::ok);
            co_return;
        });
}

TEST_CASE("Session: configurable cookie name", "[session]")
{
    mw::session_middleware sm;
    sm.cookie_name("my_session").cookie_path("/app");

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/named",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto sess = mw::fetch<mw::session_middleware>(req).value();
                    sess->set("key", "val");
                    resp.set_string_content("ok"sv, "text/plain"sv);
                },
                sm);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/named"));
            auto set_cookie = std::string(resp[httplib::field::set_cookie]);
            REQUIRE(set_cookie.starts_with("my_session="));
            REQUIRE(set_cookie.find("Path=/app") != std::string::npos);
            co_return;
        });
}

TEST_CASE("Session: cookie attributes http_only, secure, max_age", "[session]")
{
    mw::session_middleware sm;
    sm.cookie_name("attrs").http_only(true).secure(true).max_age(std::chrono::hours(1));

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/attrs",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    mw::fetch<mw::session_middleware>(req).value()->set("x", "1");
                    resp.set_string_content("ok"sv, "text/plain"sv);
                },
                sm);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/attrs"));
            auto set_cookie = std::string(resp[httplib::field::set_cookie]);
            REQUIRE(set_cookie.find("HttpOnly") != std::string::npos);
            REQUIRE(set_cookie.find("Secure") != std::string::npos);
            REQUIRE(set_cookie.find("Max-Age=3600") != std::string::npos);
            co_return;
        });
}

TEST_CASE("Session: same_site strict", "[session]")
{
    mw::session_middleware sm;
    sm.cookie_name("samesite").same_site_strict();

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/samesite",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    mw::fetch<mw::session_middleware>(req).value()->set("x", "1");
                    resp.set_string_content("ok"sv, "text/plain"sv);
                },
                sm);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/samesite"));
            auto set_cookie = std::string(resp[httplib::field::set_cookie]);
            REQUIRE(set_cookie.find("SameSite=Strict") != std::string::npos);
            co_return;
        });
}

TEST_CASE("Session: max_age cookie attribute", "[session]")
{
    mw::session_middleware sm;
    sm.cookie_name("aged").max_age(std::chrono::seconds(1800));

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/aged",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    mw::fetch<mw::session_middleware>(req).value()->set("x", "1");
                    resp.set_string_content("ok"sv, "text/plain"sv);
                },
                sm);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp = UNWRAP(co_await client.async_get("/aged"));
            auto set_cookie = std::string(resp[httplib::field::set_cookie]);
            REQUIRE(set_cookie.find("Max-Age=1800") != std::string::npos);
            co_return;
        });
}

// ===== memory_session_store 的容量与回收 =====

TEST_CASE("Session store: max_sessions caps the number of retained sessions", "[session]")
{
    mw::memory_session_store store(std::chrono::seconds(600));
    store.set_max_sessions(3);

    for (int i = 0; i < 20; ++i)
    {
        mw::session s("id-" + std::to_string(i));
        store.save(s);
    }

    REQUIRE(store.size() == 3);
}

TEST_CASE("Session store: max_sessions does not drop existing ids on update", "[session]")
{
    mw::memory_session_store store(std::chrono::seconds(600));
    store.set_max_sessions(2);

    mw::session a("a");
    a.set("k", "1");
    store.save(a);

    // 同一个 id 反复保存属于更新，不应触发淘汰。
    for (int i = 0; i < 10; ++i)
    {
        store.save(a);
    }
    REQUIRE(store.size() == 1);

    auto loaded = store.load("a");
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->get("k") == "1");
}

TEST_CASE("Session store: expired sessions are reclaimed on save", "[session]")
{
    // TTL 1s：窗口远短于测试时长，过期回收只能来自 save() 里的顺带清扫。
    mw::memory_session_store store(std::chrono::seconds(1));

    for (int i = 0; i < 5; ++i)
    {
        store.save(mw::session("old-" + std::to_string(i)));
    }
    REQUIRE(store.size() == 5);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // 顺带清扫的节流间隔是 TTL 的四分之一（下限 1s），此时应已可触发。
    store.save(mw::session("fresh"));
    REQUIRE(store.size() == 1);
    REQUIRE(store.load("fresh") != nullptr);
}

TEST_CASE("Session store: explicit cleanup reclaims expired sessions", "[session]")
{
    mw::memory_session_store store(std::chrono::seconds(1));

    store.save(mw::session("gone"));
    REQUIRE(store.size() == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    store.cleanup();
    REQUIRE(store.size() == 0);
    REQUIRE(store.load("gone") == nullptr);
}

TEST_CASE("Session store: max_sessions prefers evicting expired sessions", "[session]")
{
    mw::memory_session_store store(std::chrono::seconds(1));
    store.set_max_sessions(2);

    store.save(mw::session("a"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // 上限为 2，此刻只有 1 条（且已过期）：新条目应直接占用，不该淘汰任何东西。
    store.save(mw::session("b"));
    REQUIRE(store.size() == 1);
    REQUIRE(store.load("b") != nullptr);
}

// ===== 并发：同一session_id 的并发请求 =====

TEST_CASE("Session store: load returns a copy, not the stored instance", "[session]")
{
    // load() 曾返回 map 里那个对象的 shared_ptr 别名，于是两个携带同一 session_id 的
    // 并发请求会拿到同一个 session，在各自的 strand 上无锁写同一个 unordered_map。
    mw::memory_session_store store(std::chrono::seconds(600));
    store.save(mw::session("shared"));

    auto a = store.load("shared");
    auto b = store.load("shared");

    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a.get() != b.get());
}

TEST_CASE("Session store: concurrent writers on one session id do not lose updates", "[session]")
{
    // save() 曾整体替换节点，于是同一 id 的并发保存是 last-writer-wins：
    // 每个线程只写自己的键，最终应当全部保留。
    mw::memory_session_store store(std::chrono::seconds(600));

    constexpr int n_threads = 8;
    constexpr int n_keys = 200;

    store.save(mw::session("shared"));

    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t)
    {
        threads.emplace_back(
            [&store, t]
            {
                for (int i = 0; i < n_keys; ++i)
                {
                    // load 与 save 之间不持锁：刻意留下窗口，让并发写入真实交错。
                    auto s = store.load("shared");
                    if (!s)
                    {
                        continue;
                    }
                    s->set("t" + std::to_string(t) + "_k" + std::to_string(i), "v");
                    store.save(*s);
                }
            });
    }
    for (auto& th : threads)
    {
        th.join();
    }

    auto final = store.load("shared");
    REQUIRE(final != nullptr);
    for (int t = 0; t < n_threads; ++t)
    {
        for (int i = 0; i < n_keys; ++i)
        {
            INFO("thread " << t << " key " << i);
            REQUIRE(final->has("t" + std::to_string(t) + "_k" + std::to_string(i)));
        }
    }
}

TEST_CASE("Session store: save merges only keys this request actually touched", "[session]")
{
    // 回归测试：合并必须基于"本次请求改动过的键"，而不是遍历整张 data。
    //
    // 这里刻意不并发——两个请求**依次**保存，但都基于同一份旧基线 load 出来的副本。
    // 纯 last-writer-wins 或"遍历整个 data"的合并都会把 A 的写入冲掉；
    // 只有按 dirty 键增量合并才能同时保留 A 和 B 的结果。
    mw::memory_session_store store(std::chrono::seconds(600));

    store.save(mw::session("shared"));

    // 两个请求都在"对方保存之前"就 load 完了，各自持有同一份基线快照。
    auto a = store.load("shared");
    auto b = store.load("shared");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    a->set("a_key", "1");
    b->set("b_key", "2");

    store.save(*a);
    store.save(*b); // 后保存者不得冲掉先保存者

    auto final = store.load("shared");
    REQUIRE(final != nullptr);
    REQUIRE(final->get("a_key") == std::optional<std::string> { "1" });
    REQUIRE(final->get("b_key") == std::optional<std::string> { "2" });
}

TEST_CASE("Session store: untouched keys in a stale copy do not clobber newer writes", "[session]")
{
    // 反向场景：请求 B 没有碰 key "x"，但它的副本里带着 x 的**旧值**。
    // 若 save() 遍历整张 data，B 的保存会把 A 刚写入的新值改回旧值。
    mw::memory_session_store store(std::chrono::seconds(600));

    auto seeded = mw::session("s");
    seeded.set("x", "old");
    seeded.set("y", "untouched-by-b");
    store.save(seeded);

    auto a = store.load("s");
    auto b = store.load("s");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    a->set("x", "new");
    store.save(*a);

    b->set("something_else", "z");
    store.save(*b); // b 从未碰过 x

    auto final = store.load("s");
    REQUIRE(final != nullptr);
    REQUIRE(final->get("x") == std::optional<std::string> { "new" });
    REQUIRE(final->get("something_else") == std::optional<std::string> { "z" });
    REQUIRE(final->get("y") == std::optional<std::string> { "untouched-by-b" });
}

TEST_CASE("Session store: remove survives the merge back into the stored session", "[session]")
{
    // 合并语义必须能区分「显式删除」与「从未存在」，否则 remove() 会静默失效。
    mw::memory_session_store store(std::chrono::seconds(600));

    auto seeded = mw::session("s");
    seeded.set("keep", "1");
    seeded.set("drop", "2");
    store.save(seeded);

    auto loaded = store.load("s");
    REQUIRE(loaded != nullptr);
    loaded->remove("drop");
    store.save(*loaded);

    auto after = store.load("s");
    REQUIRE(after != nullptr);
    REQUIRE(after->has("keep"));
    REQUIRE_FALSE(after->has("drop"));
}

TEST_CASE("Session store: set after remove wins over the pending removal", "[session]")
{
    mw::memory_session_store store(std::chrono::seconds(600));

    auto seeded = mw::session("s");
    seeded.set("k", "old");
    store.save(seeded);

    auto loaded = store.load("s");
    REQUIRE(loaded != nullptr);
    loaded->remove("k");
    loaded->set("k", "new");
    store.save(*loaded);

    auto after = store.load("s");
    REQUIRE(after != nullptr);
    REQUIRE(after->get("k") == "new");
}
