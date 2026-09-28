#include "httplib/headers.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

    // 把 headers 按线序拼成 "Name=Value;Name=Value;" 便于断言顺序。
    std::string
    join(httplib::headers const& h)
    {
        std::string out;
        for (auto const& f : h.fields())
        {
            out += std::string(f.name_string());
            out += '=';
            out += f.value();
            out += ';';
        }
        return out;
    }

    std::vector<std::string>
    names(httplib::headers const& h)
    {
        std::vector<std::string> out;
        for (auto const& f : h.fields())
        {
            out.emplace_back(f.name_string());
        }
        return out;
    }

    std::vector<std::string_view>
    values_of(httplib::headers const& h, httplib::field name)
    {
        std::vector<std::string_view> out;
        for (auto const& f : h.fields())
        {
            if (f.name() == name)
            {
                out.push_back(f.value());
            }
        }
        return out;
    }

} // namespace

TEST_CASE("headers fields covers an empty collection") {
    httplib::headers h;

    CHECK(h.empty());
    CHECK(h.size() == 0);
    CHECK(join(h).empty());

    std::size_t visits = 0;
    for (auto const& f : h.fields())
    {
        (void)f;
        ++visits;
    }
    CHECK(visits == 0);
}

TEST_CASE("headers iteration follows wire order") {
    httplib::headers h;
    h.insert(httplib::field::content_type, "text/plain");
    h.insert("X-Custom", "v");
    h.insert(httplib::field::accept, "*/*");

    CHECK_FALSE(h.empty());
    CHECK(h.size() == 3);
    CHECK(join(h) == "Content-Type=text/plain;X-Custom=v;Accept=*/*;");

    // 重复头按出现顺序全部可见。
    h.insert(httplib::field::set_cookie, "a=1");
    h.insert(httplib::field::set_cookie, "b=2");
    CHECK(h.size() == 5);
    CHECK(values_of(h, httplib::field::set_cookie) ==
          std::vector<std::string_view>{ "a=1", "b=2" });

    auto const ns = names(h);
    REQUIRE(ns.size() == 5);
    CHECK(ns[0] == "Content-Type");
    CHECK(ns[1] == "X-Custom");
    CHECK(ns[2] == "Accept");
    CHECK(ns[3] == "Set-Cookie");
    CHECK(ns[4] == "Set-Cookie");
}

TEST_CASE("headers iteration after clear") {
    httplib::headers h;
    h.insert(httplib::field::host, "example.com");
    REQUIRE(h.size() == 1);

    h.clear();

    CHECK(h.empty());
    CHECK(join(h).empty());
    CHECK(std::ranges::distance(h.fields()) == 0);
}

TEST_CASE("header element exposes name and value") {
    httplib::headers h;
    h.insert(httplib::field::content_type, "application/json");

    std::size_t visits = 0;
    for (auto const& f : h.fields())
    {
        ++visits;
        CHECK(f.name() == httplib::field::content_type);
        CHECK(f.name_string() == "Content-Type");
        CHECK(f.value() == "application/json");
    }
    CHECK(visits == 1);

    // 未知头的 name 是 field::unknown，名字要看 name_string。
    httplib::headers custom;
    custom.insert("X-Weird", "1");
    for (auto const& f : custom.fields())
    {
        CHECK(f.name() == httplib::field::unknown);
        CHECK(f.name_string() == "X-Weird");
    }
}

TEST_CASE("fields() returns a single pass input range") {
    // 生成器只保证 input 语义：可解引用、可 ++、可与哨兵比较，但不能多趟遍历。
    using gen_t = decltype(std::declval<httplib::headers const&>().fields());
    STATIC_CHECK(std::is_same_v<gen_t, std::generator<httplib::header const&>>);
    STATIC_CHECK(std::ranges::range<gen_t>);
    STATIC_CHECK(!std::ranges::forward_range<gen_t>);

    httplib::headers h;
    h.insert(httplib::field::content_type, "text/plain");
    h.insert(httplib::field::accept, "*/*");
    h.insert("X-Custom", "v");

    // 每次调用 fields() 都是一个全新的生成器，所以可以反复取。
    CHECK(std::ranges::distance(h.fields()) == 3);
    CHECK(join(h) == join(h));

    // 提前 break 必须正常析构协程帧（不能崩、不能漏）。
    std::size_t seen = 0;
    for (auto const& f : h.fields())
    {
        (void)f;
        ++seen;
        break;
    }
    CHECK(seen == 1);

    // 遍历一次后生成器即耗尽，重复取 fields() 才是新的。
    auto g = h.fields();
    (void)g.begin();
    auto const first_pass = names(h);
    auto const second_pass = names(h);
    CHECK(first_pass == second_pass);
    CHECK(first_pass.size() == 3);
}

TEST_CASE("draining the generator yields every field in wire order") {
    httplib::headers h;
    h.insert(httplib::field::set_cookie, "a=1");
    h.insert(httplib::field::set_cookie, "b=2");
    h.insert("X-Custom", "v");

    // 重复头各占一条，顺序即线序。
    std::vector<httplib::header> drained;
    for (auto const& f : h.fields())
    {
        drained.push_back(f);
    }
    REQUIRE(drained.size() == 3);
    CHECK(drained[0].name() == httplib::field::set_cookie);
    CHECK(drained[0].value() == "a=1");
    CHECK(drained[1].name() == httplib::field::set_cookie);
    CHECK(drained[1].value() == "b=2");
    CHECK(drained[2].name_string() == "X-Custom");
    CHECK(drained.size() == h.size());

    // 手写 begin()/end() 时必须把生成器存成具名变量：协程帧归生成器所有，
    // `h.fields().begin()` 里那个临时生成器一析构，迭代器就悬垂了。
    auto gen = h.fields();
    auto it = gen.begin();
    auto const last = gen.end();
    std::size_t seen = 0;
    while (it != last)
    {
        ++it;
        ++seen;
    }
    CHECK(seen == 3);
}

TEST_CASE("headers copy is independent, view writes through") {
    httplib::headers h;
    h.insert(httplib::field::content_type, "text/plain");
    h.insert(httplib::field::set_cookie, "a=1");
    h.insert(httplib::field::set_cookie, "b=2");

    auto copy = h;
    copy.set(httplib::field::content_type, "application/json");

    CHECK(h[httplib::field::content_type] == "text/plain");
    CHECK(copy[httplib::field::content_type] == "application/json");
    CHECK(h.size() == 3);
    CHECK(copy.size() == 3);

    // set() 跟随 beast 的位置语义：先删同名再插入，被覆盖的字段移到末尾。
    CHECK(join(h) == "Content-Type=text/plain;Set-Cookie=a=1;Set-Cookie=b=2;");
    CHECK(join(copy) == "Set-Cookie=a=1;Set-Cookie=b=2;Content-Type=application/json;");
}
