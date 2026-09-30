#include "httplib/server/request_data.hpp"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

using httplib::server::request_data;

TEST_CASE("request_data fetch returns the stored value", "[request_data]")
{
    request_data data;
    data.store(42);

    auto r = data.fetch<int>();
    REQUIRE(r.has_value());
    REQUIRE(r.value() == 42);
}

TEST_CASE("request_data fetch reports a missing key as nullopt", "[request_data]")
{
    request_data data;

    REQUIRE(!data.fetch<int>().has_value());
    REQUIRE(data.fetch<int>() == std::nullopt);
    // value_or 提供默认值，避免调用方手写 has_value() 分支。
    REQUIRE(data.fetch<int>().value_or(-1) == -1);
}

TEST_CASE("request_data fetch distinguishes types and tags", "[request_data]")
{
    request_data data;
    data.store(std::string { "no-tag" });
    data.store("tagged", std::string { "with-tag" });

    REQUIRE(data.fetch<std::string>().value() == "no-tag");
    REQUIRE(data.fetch<std::string>("tagged").value() == "with-tag");

    // 键里含 typeid：类型不同即视为不存在。
    REQUIRE(!data.fetch<int>().has_value());
    // tag 不同同样不存在。
    REQUIRE(!data.fetch<std::string>("other").has_value());
}

TEST_CASE("request_data lookups ignore cv-qualifiers on the requested type", "[request_data]")
{
    // store() 存键时用的是 decay_t<T>，所以存进去的是"去修饰"的类型。取值侧必须
    // 用同一套规则，否则 fetch<const int>() 会去查 typeid(const int) 这个不同的键，
    // 静默 miss——这正是把 decay 收进 make_key 的原因。
    request_data data;
    data.store(42);

    REQUIRE(data.fetch<int const>().has_value());
    // has() 不取值，键与 fetch 共用 decay 规则，故命中同一个键。
    REQUIRE(data.has<int const>());
    data.erase<int const>();
    REQUIRE(!data.has<int>());
    REQUIRE(!data.fetch<int>().has_value());

    // volatile 同样被抹掉。
    data.store(7);
    REQUIRE(data.fetch<int volatile>().value() == 7);

    // 引用类型也 decay 成值类型：返回 int 副本，而不是 optional<int&>。
    int x = 5;
    data.store(x);
    STATIC_REQUIRE(std::is_same_v<decltype(data.fetch<int&>()), std::optional<int>>);
    REQUIRE(data.fetch<int&>().value() == 5);
    REQUIRE(data.fetch<int const&>().value() == 5);
}

TEST_CASE("request_data fetch copies without consuming", "[request_data]")
{
    request_data data;

    auto sp = std::make_shared<int>(7);
    data.store(sp);

    // 只有 const 版本，且一律拷贝：两次取值后存储里的 shared_ptr 仍然有效，
    // 说明没有发生"从 any 里搬走"的移动。
    auto r1 = data.fetch<std::shared_ptr<int>>();
    REQUIRE(r1.has_value());
    REQUIRE(r1.value() == sp);

    auto r2 = data.fetch<std::shared_ptr<int>>();
    REQUIRE(r2.has_value());
    REQUIRE(r2.value() == sp);
    REQUIRE(r2.value().use_count() >= 2);

    // 存储中的值没有被前一次取值搬空。
    REQUIRE(data.fetch<std::shared_ptr<int>>().value() == sp);
    REQUIRE(data.fetch<std::shared_ptr<int>>().has_value());
}

TEST_CASE("request_data fetch sees stores that happen after a failed lookup", "[request_data]")
{
    request_data data;

    REQUIRE(!data.fetch<int>().has_value());
    data.store(1);
    REQUIRE(data.fetch<int>().has_value());

    data.erase<int>();
    REQUIRE(!data.fetch<int>().has_value());
}

TEST_CASE("request_data fetch is atomic per call under concurrent erase", "[request_data]")
{
    // fetch 的保证范围是**单次调用**：一次加锁内完成查找+取值，所以返回时要么拿到
    // 一个完整值，要么明确拿到 nullopt，绝不会抛异常、也不会读到撕裂的数据。
    //
    // 这与 has()+取值有本质区别：那是两次加锁，中间可被 erase 打断。本测试只断言
    // fetch 自身不抛、不返回错值——不断言它与 has() 的结果一致，因为那本来就无法
    // 保证，而那正是需要 fetch 的原因。
    request_data data;
    data.store(5);

    constexpr int n_iters = 4000;
    std::atomic<bool> stop { false };
    std::atomic<int> bad_reads { 0 };
    std::atomic<int> hits { 0 };

    std::thread toggler(
        [&]
        {
            for (int i = 0; i < n_iters; ++i)
            {
                data.erase<int>();
                data.store(5);
            }
            stop.store(true);
        });

    for (int i = 0; i < n_iters && !stop.load(); ++i)
    {
        // 关键：此处只有 fetch 这一次加锁，不与任何其他调用交错。
        auto r = data.fetch<int>();
        if (r.has_value())
        {
            if (r.value() != 5)
            {
                bad_reads.fetch_add(1); // 读到了非 5：不该发生
            }
            hits.fetch_add(1);
        }
    }
    toggler.join();

    REQUIRE(bad_reads.load() == 0);
    // 确认并发确实发生过，否则本例会退化成单线程空跑。
    REQUIRE(hits.load() > 0);
}