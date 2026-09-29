#include "enum_conv.hpp"
#include <catch2/catch_test_macros.hpp>

// 名字表在 httplib::enum_conv，Beast 侧类型经 httplib::http 暴露；测试用例统一写
// enum_conv:: / http::，靠这一行引入，不必逐处写全限定名。
using namespace httplib;

// enum_conv.hpp 原来带 908 条逐值 static_assert，与两个方向的 case 同源、恒真，
// 改成 X-macro 名字表后已删除。覆盖性改由本文件兜：名字表漏掉一个枚举值时，该值
// 会静默退回末尾的兜底 return，这里逐值检查即可发现。
//
// 加一个枚举值要改三处：include/httplib/enums.hpp、lib/enum_conv.hpp 里对应的名字
// 表、以及下面的 kMethodCount / kFieldCount / kStatusCodes。第三处是故意的——数字对
// 不上会让测试失败，强迫改表的人回头确认映射，而不是让多出来的枚举值安静退化成
// unknown。

namespace
{
    // method 与 field 的枚举值连续（0 起逐个递增），可按范围穷举。
    constexpr unsigned kMethodCount = 34;
    constexpr unsigned kFieldCount = 357;

    // status 的枚举值就是真实 HTTP 状态码，不连续（200 / 404 / 503 ...），无法按范围
    // 穷举，只能显式列出——这份列表同时把 enums.hpp 里的数值钉在标准码上，那些数字
    // 从不直接上线，但填错仍会让日志与错误码失真。enums.hpp 若新增状态码，此表须同步。
    constexpr unsigned kStatusCodes[] = {
        0,   100, 101, 102, 103, 200, 201, 202, 203, 204, 205, 206, 207, 208, 226, 300, 301, 302, 303, 304,
        305, 307, 308, 400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 410, 411, 412, 413, 414, 415,
        416, 417, 418, 421, 422, 423, 424, 425, 426, 428, 429, 431, 451, 500, 501, 502, 503, 504, 505,
        506, 507, 508, 510, 511
    };

    // 逐值检查一个方向的全覆盖 + 往返一致。fwd / rev 用 lambda 传入而非函数指针：
    // to_field 与 to_status 都是重载集，模板无法从重载集推导。
    template <typename Mine, typename Theirs, typename Fwd, typename Rev>
    void check_pair(unsigned count, Fwd fwd, Rev rev)
    {
        for (unsigned i = 0; i < count; ++i)
        {
            auto const mine = static_cast<Mine>(i);
            auto const theirs = fwd(mine);

            CAPTURE(i);
            // 0 是 unknown，自映射到 unknown 是对的；其余值若也落到 unknown，说明
            // 名字表漏了这一行。
            if (i != 0)
            {
                CHECK(theirs != Theirs::unknown);
            }
            CHECK(rev(theirs) == mine);
        }
    }
} // namespace

TEST_CASE("enum_conv: method 与 http::verb 双向全覆盖")
{
    check_pair<httplib::method, http::verb>(
        kMethodCount, [](httplib::method v) { return enum_conv::to_verb(v); },
        [](http::verb v) { return enum_conv::to_method(v); });
}

TEST_CASE("enum_conv: field 与 http::field 双向全覆盖")
{
    check_pair<httplib::field, http::field>(
        kFieldCount, [](httplib::field v) { return enum_conv::to_field(v); },
        [](http::field v) { return enum_conv::to_field(v); });
}

TEST_CASE("enum_conv: status 与 http::status 双向全覆盖，且数值是标准 HTTP 码")
{
    for (auto const code : kStatusCodes)
    {
        auto const mine = static_cast<httplib::status>(code);
        auto const theirs = enum_conv::to_status(mine);

        CAPTURE(code);
        if (code != 0)
        {
            CHECK(theirs != http::status::unknown);
        }
        CHECK(enum_conv::to_status(theirs) == mine);
    }
}

TEST_CASE("enum_conv: 枚举外的数值退回 unknown")
{
    // 与 enum_conv.hpp 里那六条 static_assert 同一件事，从调用方角度再验一次，确认
    // 兜底 return 没被改成 UB。
    CHECK(enum_conv::to_verb(static_cast<httplib::method>(9999)) == http::verb::unknown);
    CHECK(enum_conv::to_method(static_cast<http::verb>(9999)) == httplib::method::unknown);
    CHECK(enum_conv::to_status(static_cast<httplib::status>(9999)) == http::status::unknown);
    CHECK(enum_conv::to_status(static_cast<http::status>(9999)) == httplib::status::unknown);
    CHECK(enum_conv::to_field(static_cast<httplib::field>(9999)) == http::field::unknown);
    CHECK(enum_conv::to_field(static_cast<http::field>(9999)) == httplib::field::unknown);
}
