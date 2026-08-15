#pragma once

#include "fwd.hpp"
#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace httplib::db
{

    /**
     * \brief 日期（年/月/日）。
     */
    struct date
    {
        unsigned year = 0, month = 0, day = 0;

        /// 是否为合法日期（含闰年/大小月校验）。
        constexpr bool is_valid() const noexcept;
        /// 是否闰年。
        constexpr bool is_leap_year() const noexcept;
        /// 该年该月的天数（month 非法返回 0）。
        constexpr unsigned days_in_month() const noexcept;

        /// 转为 std::chrono::sys_days（自纪元起的天数）。
        constexpr std::chrono::sys_days to_sys_days() const;
        /// 由 std::chrono::sys_days 构造。
        static constexpr date from_sys_days(std::chrono::sys_days ds) noexcept;

        constexpr date& operator+=(std::chrono::days n);
        constexpr date& operator-=(std::chrono::days n);

        /// 格式化为 ISO 8601（YYYY-MM-DD）。
        std::string to_string() const;
        /// 解析 ISO 8601（YYYY-MM-DD），非法返回 nullopt。
        static std::optional<date> from_string(std::string_view sv);

        auto operator<=>(date const&) const = default;
        bool operator==(date const&) const = default;
    };

    constexpr date operator+(date d, std::chrono::days n);
    constexpr date operator-(date d, std::chrono::days n);
    constexpr std::chrono::days operator-(date const& a, date const& b);

    /**
     * \brief 时间（时/分/秒/微秒）。
     */
    struct time
    {
        unsigned hour = 0, minute = 0, second = 0;
        unsigned long microsecond = 0;
        bool negative = false; ///< 是否整体为负（负 TIME）。

        /// 是否为合法时间（hour 可为任意非负值，分钟/秒 < 60，微秒 < 1e6）。
        constexpr bool is_valid() const noexcept;
        /// 转为 std::chrono::microseconds 时长（可为负）。
        constexpr std::chrono::microseconds to_duration() const noexcept;
        /// 总微秒数（可为负）。
        constexpr int64_t total_microseconds() const noexcept;
        /// 由时长构造（支持负值）。
        static constexpr time from_duration(std::chrono::microseconds d) noexcept;
        /// 格式化为 [-]HH:MM:SS[.ffffff]。
        std::string to_string() const;
        /// 解析 [-]HH:MM:SS[.ffffff]，非法返回 nullopt。
        static std::optional<time> from_string(std::string_view sv);

        constexpr std::strong_ordering operator<=>(time const& other) const noexcept;
        constexpr bool operator==(time const& other) const noexcept;
    };

    /**
     * \brief 日期时间（date 与 time 的组合）。
     */
    struct datetime
        : date
        , time
    {
        /// 是否为合法日期时间。
        constexpr bool is_valid() const noexcept;
        /// 转为 std::chrono::system_clock::time_point。
        constexpr std::chrono::system_clock::time_point to_time_point() const;
        /// 由 time_point 构造。
        static constexpr datetime from_time_point(std::chrono::system_clock::time_point tp) noexcept;
        /// 格式化为 YYYY-MM-DD HH:MM:SS[.ffffff]。
        std::string to_string() const;
        /// 解析 YYYY-MM-DD HH:MM:SS[.ffffff]，非法返回 nullopt。
        static std::optional<datetime> from_string(std::string_view sv);

        auto operator<=>(datetime const&) const = default;
        bool operator==(datetime const&) const = default;
    };

    /// 两个日期时间的差值（微秒）。
    constexpr std::chrono::microseconds operator-(datetime const& a, datetime const& b);

} // namespace httplib::db

#include "temporal.inl"
