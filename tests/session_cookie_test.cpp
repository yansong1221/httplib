#include "common.hpp"
#include "httplib/server/middleware/data.hpp"
#include "httplib/server/middleware/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "server/middleware/memory_store.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mw = httplib::server::middleware;
namespace net = httplib::net;
namespace util = httplib::util;

namespace
{
    using test_common::as_string;
    using test_common::run;
    using test_common::setup_logger;

    /// 只含覆盖写的补丁。
    mw::session_writes
    patch(std::initializer_list<std::pair<std::string const, std::string const>> kvs)
    {
        mw::session_writes w;
        for (auto const& kv : kvs)
        {
            w.set(kv.first, kv.second);
        }
        return w;
    }

    /// 真正由外部实现的 store：证明 `session_store` 的契约（按值快照 + 增量提交）
    /// 足以在不接触 `session` 任何内部状态的前提下写出一个后端。锁是契约的一部分 ——
    /// save 必须与同一 id 的并发 save 原子化。
    class counting_store : public mw::session_store
    {
      public:
        std::optional<mw::session>
        load(std::string_view id) override
        {
            std::lock_guard lock(mutex_);
            ++loads_;
            auto it = data_.find(std::string(id));
            if (it == data_.end())
            {
                return std::nullopt;
            }
            return mw::session(it->first, created_, created_, it->second);
        }

        void
        save(std::string_view id, mw::session_writes const& writes) override
        {
            std::lock_guard lock(mutex_);
            ++saves_;
            auto& slot = data_[std::string(id)];
            for (auto const& [key, value] : writes.entries())
            {
                if (value)
                {
                    slot.insert_or_assign(key, *value);
                }
                else
                {
                    slot.erase(key);
                }
            }
        }

        void
        destroy(std::string_view id) override
        {
            std::lock_guard lock(mutex_);
            data_.erase(std::string(id));
        }

        std::size_t
        loads() const
        {
            return loads_;
        }

        std::size_t
        saves() const
        {
            return saves_;
        }

      private:
        mutable std::mutex mutex_;
        util::string_map<util::string_map<std::string>> data_;
        mw::session::time_point created_ = mw::session::clock::now();
        std::size_t loads_ = 0;
        std::size_t saves_ = 0;
    };

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

TEST_CASE("Session: a custom store can be injected", "[session]")
{
    auto store = std::make_shared<counting_store>();
    mw::session_middleware sm(store);

    run(
        [&](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/custom",
                [](httplib::server::request& req, httplib::server::response& resp)
                {
                    auto sess = mw::fetch<mw::session_middleware>(req).value();
                    // 第二个请求带 cookie 进来时，写入必须能经 store 的 load 读回。
                    if (auto v = sess->get("store"); v)
                    {
                        resp.set_string_content(*v, "text/plain"sv);
                        return;
                    }
                    sess->set("store", "injected");
                    resp.set_string_content("fresh"sv, "text/plain"sv);
                },
                sm);
        },
        [](auto& client) -> net::awaitable<void>
        {
            auto resp1 = UNWRAP(co_await client.async_get("/custom"));
            REQUIRE(resp1.result() == httplib::status::ok);
            REQUIRE(as_string(resp1) == "fresh");
            auto cookie = std::string(resp1[httplib::field::set_cookie]);
            REQUIRE_FALSE(cookie.empty());

            auto hdrs = httplib::headers();
            hdrs.set(httplib::field::cookie, cookie);

            auto resp2 = UNWRAP(co_await client.async_get("/custom", {}, hdrs));
            REQUIRE(resp2.result() == httplib::status::ok);
            REQUIRE(as_string(resp2) == "injected");
            co_return;
        });

    // 第二个请求确实走了 store 的 load，而不是命中什么缓存。
    REQUIRE(store->loads() == 1);
    REQUIRE(store->saves() == 2);
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
        store.save("id-" + std::to_string(i), mw::session_writes {});
    }

    REQUIRE(store.size() == 3);
}

TEST_CASE("Session store: max_sessions does not drop existing ids on update", "[session]")
{
    mw::memory_session_store store(std::chrono::seconds(600));
    store.set_max_sessions(2);

    store.save("a",
               patch({
                   { "k", "1" }
    }));

    // 同一个 id 反复保存属于更新，不应触发淘汰。
    for (int i = 0; i < 10; ++i)
    {
        store.save("a",
                   patch({
                       { "k", "1" }
        }));
    }
    REQUIRE(store.size() == 1);

    auto loaded = store.load("a");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->get("k") == "1");
}

TEST_CASE("Session store: expired sessions are reclaimed on save", "[session]")
{
    // TTL 1s：窗口远短于测试时长，过期回收只能来自 save() 里的顺带清扫。
    mw::memory_session_store store(std::chrono::seconds(1));

    for (int i = 0; i < 5; ++i)
    {
        store.save("old-" + std::to_string(i), mw::session_writes {});
    }
    REQUIRE(store.size() == 5);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // 顺带清扫的节流间隔是 TTL 的四分之一（下限 1s），此时应已可触发。
    store.save("fresh", mw::session_writes {});
    REQUIRE(store.size() == 1);
    REQUIRE(store.load("fresh").has_value());
}

TEST_CASE("Session store: explicit cleanup reclaims expired sessions", "[session]")
{
    mw::memory_session_store store(std::chrono::seconds(1));

    store.save("gone", mw::session_writes {});
    REQUIRE(store.size() == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    store.cleanup();
    REQUIRE(store.size() == 0);
    REQUIRE_FALSE(store.load("gone").has_value());
}

TEST_CASE("Session store: max_sessions prefers evicting expired sessions", "[session]")
{
    mw::memory_session_store store(std::chrono::seconds(1));
    store.set_max_sessions(2);

    store.save("a", mw::session_writes {});
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // 上限为 2，此刻只有 1 条（且已过期）：新条目应直接占用，不该淘汰任何东西。
    store.save("b", mw::session_writes {});
    REQUIRE(store.size() == 1);
    REQUIRE(store.load("b").has_value());
}

// ===== 并发：同一session_id 的并发请求 =====

TEST_CASE("Session store: load returns a value snapshot, not a handle to stored state", "[session]")
{
    // load() 曾返回 map 里那个对象的 shared_ptr 别名，于是两个携带同一 session_id 的
    // 并发请求会拿到同一个 session，在各自的 strand 上无锁写同一个 unordered_map。
    // 现在按值返回 optional<session>，共享可变状态在类型上就不可能发生。
    using load_result = decltype(std::declval<mw::memory_session_store&>().load(std::string_view {}));
    STATIC_REQUIRE(std::is_same_v<load_result, std::optional<mw::session>>);

    mw::memory_session_store store(std::chrono::seconds(600));
    store.save("shared",
               patch({
                   { "seed", "1" }
    }));

    auto a = store.load("shared");
    auto b = store.load("shared");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    // 两份快照互不影响：改 a 不会波及 b。
    *a = mw::session("mutated");
    REQUIRE(a->id() == "mutated");
    REQUIRE(b->id() == "shared");
    REQUIRE(b->get("seed") == std::optional<std::string> { "1" });
}

TEST_CASE("Session store: concurrent writers on one session id do not lose updates", "[session]")
{
    // save() 曾整体替换节点，于是同一 id 的并发保存是 last-writer-wins：
    // 每个线程只写自己的键，最终应当全部保留。
    mw::memory_session_store store(std::chrono::seconds(600));

    constexpr int n_threads = 8;
    constexpr int n_keys = 200;

    store.save("shared", mw::session_writes {});

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
                    auto snapshot = store.load("shared");
                    if (!snapshot)
                    {
                        continue;
                    }
                    snapshot->set("t" + std::to_string(t) + "_k" + std::to_string(i), "v");
                    store.save(snapshot->id(), snapshot->take_pending_writes());
                }
            });
    }
    for (auto& th : threads)
    {
        th.join();
    }

    auto final = store.load("shared");
    REQUIRE(final.has_value());
    for (int t = 0; t < n_threads; ++t)
    {
        for (int i = 0; i < n_keys; ++i)
        {
            INFO("thread " << t << " key " << i);
            REQUIRE(final->has("t" + std::to_string(t) + "_k" + std::to_string(i)));
        }
    }
}

TEST_CASE("Session store: a stale snapshot only submits the keys it touched", "[session]")
{
    // 回归测试：提交的内容必须基于「本次请求改动过的键」，而不是整张基线 data。
    //
    // 这里刻意不并发 —— 两个请求**依次**保存，但都基于同一份基线 load 出来的快照。
    // 若 store 用快照整体替换存量（或把整张 data 当成增量），后保存者就会抹掉
    // 先保存者的写入；只有逐键应用补丁才能同时保留 A 和 B 的结果。
    mw::memory_session_store store(std::chrono::seconds(600));

    store.save("shared", mw::session_writes {});

    // 两个请求都在「对方保存之前」就 load 完了，各自持有同一份基线快照。
    auto a = store.load("shared");
    auto b = store.load("shared");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    a->set("a_key", "1");
    b->set("b_key", "2");

    store.save(a->id(), a->take_pending_writes());
    store.save(b->id(), b->take_pending_writes()); // 后保存者不得冲掉先保存者

    auto final = store.load("shared");
    REQUIRE(final.has_value());
    REQUIRE(final->get("a_key") == std::optional<std::string> { "1" });
    REQUIRE(final->get("b_key") == std::optional<std::string> { "2" });
}

TEST_CASE("Session store: untouched keys in a stale snapshot do not clobber newer writes", "[session]")
{
    // 增量提交的关键性质：快照里带着 x 的**旧值**，但本次请求没碰过 x，
    // 于是 x 根本不进入补丁，store 也就无从用它覆盖 A 刚写入的新值。
    mw::memory_session_store store(std::chrono::seconds(600));

    store.save("s",
               patch({
                   { "x",            "old" },
                   { "y", "untouched-by-b" }
    }));

    auto a = store.load("s");
    auto b = store.load("s");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    a->set("x", "new");
    store.save(a->id(), a->take_pending_writes());

    b->set("something_else", "z");
    store.save(b->id(), b->take_pending_writes()); // b 从未碰过 x

    auto final = store.load("s");
    REQUIRE(final.has_value());
    REQUIRE(final->get("x") == std::optional<std::string> { "new" });
    REQUIRE(final->get("something_else") == std::optional<std::string> { "z" });
    REQUIRE(final->get("y") == std::optional<std::string> { "untouched-by-b" });
}

TEST_CASE("Session store: remove reaches the store as an explicit deletion", "[session]")
{
    // 删除必须能被显式表达，否则 store 无从区分「从未存在」与「已被删除」，
    // remove() 提交后会静默失效。
    mw::memory_session_store store(std::chrono::seconds(600));

    store.save("s",
               patch({
                   { "keep", "1" },
                   { "drop", "2" }
    }));

    auto loaded = store.load("s");
    REQUIRE(loaded.has_value());
    loaded->remove("drop");

    // 补丁里确实带着这条删除，而不是一条空补丁。
    REQUIRE(loaded->pending_writes().size() == 1);
    REQUIRE(loaded->pending_writes().entries().contains("drop"));
    REQUIRE_FALSE(loaded->pending_writes().entries().at("drop").has_value());

    store.save(loaded->id(), loaded->take_pending_writes());

    auto after = store.load("s");
    REQUIRE(after.has_value());
    REQUIRE(after->has("keep"));
    REQUIRE_FALSE(after->has("drop"));
}

TEST_CASE("Session store: set after remove wins over the pending removal", "[session]")
{
    mw::memory_session_store store(std::chrono::seconds(600));

    store.save("s",
               patch({
                   { "k", "old" }
    }));

    auto loaded = store.load("s");
    REQUIRE(loaded.has_value());
    loaded->remove("k");
    loaded->set("k", "new");
    store.save(loaded->id(), loaded->take_pending_writes());

    auto after = store.load("s");
    REQUIRE(after.has_value());
    REQUIRE(after->get("k") == "new");
}

TEST_CASE("Session store: take_pending_writes drains the patch", "[session]")
{
    // save 是唯一的提交点；取出后必须已清空，否则同一批写入会被重复应用。
    mw::memory_session_store store(std::chrono::seconds(600));

    auto loaded = store.load("nothing");
    REQUIRE_FALSE(loaded.has_value());

    auto s = mw::session("s");
    s.set("k", "1");
    REQUIRE(s.pending_writes().size() == 1);
    REQUIRE_FALSE(s.pending_writes().empty());

    auto drained = s.take_pending_writes();
    REQUIRE(drained.size() == 1);
    REQUIRE(s.pending_writes().empty());

    store.save(s.id(), std::move(drained));
    REQUIRE(store.load("s").has_value());
}

TEST_CASE("Session store: destroy is not sticky across a later save", "[session]")
{
    // 「增量提交」契约的直接推论：store 不记录「已删除」这一事实，只应用补丁里的键，
    // 因此 destroy 之后若仍有快照带着补丁提交，会话会被重建。
    mw::memory_session_store store(std::chrono::seconds(600));

    store.save("s", mw::session_writes {});
    auto stale = store.load("s");
    REQUIRE(stale.has_value());
    stale->set("k", "1");

    store.destroy("s");
    REQUIRE_FALSE(store.load("s").has_value());

    store.save(stale->id(), stale->take_pending_writes());
    auto back = store.load("s");
    REQUIRE(back.has_value());
    REQUIRE(back->get("k") == std::optional<std::string> { "1" });
}
