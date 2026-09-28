#include "httplib/headers.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <iterator>
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
        for (auto const& f : h)
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
        for (auto const& f : h)
        {
            out.emplace_back(f.name_string());
        }
        return out;
    }

    std::vector<std::string_view>
    values_of(httplib::headers const& h, httplib::field name)
    {
        std::vector<std::string_view> out;
        for (auto const& f : h)
        {
            if (f.name() == name)
            {
                out.push_back(f.value());
            }
        }
        return out;
    }

} // namespace

TEST_CASE("headers begin and end cover an empty collection") {
    httplib::headers h;

    CHECK(h.empty());
    CHECK(h.size() == 0);
    CHECK(h.begin() == h.end());
    CHECK(h.cbegin() == h.cend());
    CHECK(std::distance(h.begin(), h.end()) == 0);
    CHECK(join(h).empty());
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
    CHECK(h.begin() == h.end());
    CHECK(std::distance(h.begin(), h.end()) == 0);
}

TEST_CASE("header element exposes name and value") {
    httplib::headers h;
    h.insert(httplib::field::content_type, "application/json");

    auto it = h.begin();
    REQUIRE(it != h.end());

    // 解引用返回按值快照，连续解两次结果一致。
    auto first = *it;
    auto second = *it;
    CHECK(first.name() == httplib::field::content_type);
    CHECK(first.name_string() == "Content-Type");
    CHECK(first.value() == "application/json");
    CHECK(first.value() == second.value());

    // 未知头的 name 是 field::unknown，名字要看 name_string。
    httplib::headers custom;
    custom.insert("X-Weird", "1");
    CHECK((*custom.begin()).name() == httplib::field::unknown);
    CHECK((*custom.begin()).name_string() == "X-Weird");
}

TEST_CASE("header_iterator copies are independent cursors") {
    httplib::headers h;
    h.insert(httplib::field::content_type, "text/plain");
    h.insert(httplib::field::accept, "*/*");

    auto a = h.begin();
    auto b = a;
    CHECK(a == b);
    CHECK(a->value() == "text/plain");

    ++b;
    CHECK(a != b);
    CHECK(a->value() == "text/plain");
    CHECK(b->value() == "*/*");

    // 赋值也要拷贝游标。
    auto c = a;
    c = b;
    CHECK(c == b);
    CHECK(c != a);

    // 后置自增返回旧位置。
    auto d = a++;
    CHECK(d == h.begin());
    CHECK(a == b);

    // 自增 a 不影响 b / c。
    CHECK(b == c);
}

TEST_CASE("header_iterator is a forward iterator") {
    using traits = std::iterator_traits<httplib::header_iterator>;
    STATIC_CHECK(std::is_same_v<traits::iterator_category, std::forward_iterator_tag>);
    STATIC_CHECK(std::is_same_v<traits::value_type, httplib::header>);
    STATIC_CHECK(std::is_same_v<traits::reference, httplib::header>);
    STATIC_CHECK(std::is_same_v<traits::pointer, void>);

    httplib::headers h;
    h.insert(httplib::field::content_type, "text/plain");
    h.insert(httplib::field::accept, "*/*");
    h.insert("X-Custom", "v");

    // 前向迭代器：多趟遍历得到同一结果。
    CHECK(static_cast<std::size_t>(std::distance(h.begin(), h.end())) == h.size());

    auto const first_pass = names(h);
    auto const second_pass = names(h);
    CHECK(first_pass == second_pass);

    // std::find 能配合解引用结果使用。
    auto it = std::find_if(h.begin(), h.end(), [](httplib::header const& f) {
        return f.name() == httplib::field::accept;
    });
    REQUIRE(it != h.end());
    CHECK(it->value() == "*/*");
}

TEST_CASE("header_iterator default constructed is singular") {
    httplib::header_iterator def;

    // 空值迭代器只和自己相等；跟任何真实迭代器（含 end）都不相等，
    // 否则用默认构造迭代器跑循环会直接判等结束。
    httplib::header_iterator other;
    CHECK(def == other);
    CHECK_FALSE(def != other);

    httplib::headers h;
    h.insert(httplib::field::host, "example.com");

    CHECK_FALSE(def == h.begin());
    CHECK_FALSE(def == h.end());
    CHECK_FALSE(h.begin() == def);
    CHECK_FALSE(h.end() == def);
    CHECK(h.begin() != h.end());
}

TEST_CASE("headers all matches iteration") {
    httplib::headers h;
    h.insert(httplib::field::set_cookie, "a=1");
    h.insert(httplib::field::set_cookie, "b=2");
    h.insert("X-Custom", "v");

    auto const all = h.all();
    REQUIRE(all.size() == 3);

    auto it = h.begin();
    for (auto const& f : all)
    {
        REQUIRE(it != h.end());
        CHECK(f.name() == it->name());
        CHECK(f.name_string() == it->name_string());
        CHECK(f.value() == it->value());
        ++it;
    }
    CHECK(it == h.end());
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
