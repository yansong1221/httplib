#pragma once
#include <charconv>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <system_error>

namespace httplib::db
{

    constexpr bool
    date::is_valid() const noexcept
    {
        if (month < 1 || month > 12 || day < 1)
        {
            return false;
        }
        return day <= days_in_month();
    }

    constexpr bool
    date::is_leap_year() const noexcept
    {
        return std::chrono::year { static_cast<int>(year) }.is_leap();
    }

    constexpr unsigned
    date::days_in_month() const noexcept
    {
        if (month < 1 || month > 12)
        {
            return 0;
        }
        std::chrono::year_month_day_last ymdl { std::chrono::year { static_cast<int>(year) },
                                                std::chrono::month_day_last { std::chrono::month { month } } };
        return static_cast<unsigned>(ymdl.day());
    }

    constexpr std::chrono::sys_days
    date::to_sys_days() const
    {
        if (!is_valid())
        {
            throw std::runtime_error("db: invalid date value");
        }
        return std::chrono::sys_days { std::chrono::year { static_cast<int>(year) } / std::chrono::month { month }
                                       / std::chrono::day { day } };
    }

    constexpr date
    date::from_sys_days(std::chrono::sys_days ds) noexcept
    {
        std::chrono::year_month_day ymd { ds };
        return date { static_cast<unsigned>(static_cast<int>(ymd.year())),
                      static_cast<unsigned>(ymd.month()),
                      static_cast<unsigned>(ymd.day()) };
    }

    constexpr date&
    date::operator+=(std::chrono::days n)
    {
        *this = from_sys_days(to_sys_days() + n);
        return *this;
    }

    constexpr date&
    date::operator-=(std::chrono::days n)
    {
        *this = from_sys_days(to_sys_days() - n);
        return *this;
    }

    inline std::string
    date::to_string() const
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", year, month, day);
        return buf;
    }

    inline std::optional<date>
    date::from_string(std::string_view sv)
    {
        auto const* p = sv.data();
        auto const* end = sv.data() + sv.size();

        auto parse_uint = [&](unsigned& out) -> bool
        {
            if (p == end || *p < '0' || *p > '9')
            {
                return false;
            }
            auto res = std::from_chars(p, end, out);
            if (res.ec != std::errc {})
            {
                return false;
            }
            p = res.ptr;
            return true;
        };

        unsigned y = 0, m = 0, d = 0;
        if (!parse_uint(y) || p == end || *p != '-')
        {
            return std::nullopt;
        }
        ++p;
        if (!parse_uint(m) || p == end || *p != '-')
        {
            return std::nullopt;
        }
        ++p;
        if (!parse_uint(d) || p != end)
        {
            return std::nullopt;
        }

        date r { y, m, d };
        return r.is_valid() ? std::optional<date>(r) : std::nullopt;
    }

    constexpr date
    operator+(date d, std::chrono::days n)
    {
        d += n;
        return d;
    }

    constexpr date
    operator-(date d, std::chrono::days n)
    {
        d -= n;
        return d;
    }

    constexpr std::chrono::days
    operator-(date const& a, date const& b)
    {
        return a.to_sys_days() - b.to_sys_days();
    }

    constexpr bool
    time::is_valid() const noexcept
    {
        return minute < 60 && second < 60 && microsecond < 1000000;
    }

    constexpr std::chrono::microseconds
    time::to_duration() const noexcept
    {
        auto d = std::chrono::hours { hour } + std::chrono::minutes { minute } + std::chrono::seconds { second }
                 + std::chrono::microseconds { microsecond };
        return negative ? -d : d;
    }

    constexpr int64_t
    time::total_microseconds() const noexcept
    {
        return to_duration().count();
    }

    constexpr time
    time::from_duration(std::chrono::microseconds d) noexcept
    {
        bool neg = d.count() < 0;
        if (neg)
        {
            d = -d;
        }
        auto s = std::chrono::duration_cast<std::chrono::seconds>(d).count();
        return time { static_cast<unsigned>(s / 3600),
                      static_cast<unsigned>((s % 3600) / 60),
                      static_cast<unsigned>(s % 60),
                      static_cast<unsigned long>(d.count() % 1000000),
                      neg };
    }

    inline std::string
    time::to_string() const
    {
        char buf[40];
        if (microsecond)
        {
            std::snprintf(buf,
                          sizeof(buf),
                          "%s%02u:%02u:%02u.%06lu",
                          negative ? "-" : "",
                          hour,
                          minute,
                          second,
                          microsecond);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%s%02u:%02u:%02u", negative ? "-" : "", hour, minute, second);
        }
        return buf;
    }

    inline std::optional<time>
    time::from_string(std::string_view sv)
    {
        bool neg = false;
        if (!sv.empty() && sv.front() == '-')
        {
            neg = true;
            sv.remove_prefix(1);
        }

        auto const* p = sv.data();
        auto const* end = sv.data() + sv.size();

        auto parse_uint = [&](unsigned& out) -> bool
        {
            if (p == end || *p < '0' || *p > '9')
            {
                return false;
            }
            auto res = std::from_chars(p, end, out);
            if (res.ec != std::errc {})
            {
                return false;
            }
            p = res.ptr;
            return true;
        };

        unsigned h = 0, m = 0, sec = 0;
        if (!parse_uint(h) || p == end || *p != ':')
        {
            return std::nullopt;
        }
        ++p;
        if (!parse_uint(m) || p == end || *p != ':')
        {
            return std::nullopt;
        }
        ++p;
        if (!parse_uint(sec))
        {
            return std::nullopt;
        }

        unsigned long us = 0;
        if (p < end && *p == '.')
        {
            ++p;
            auto const* start = p;
            while (p < end && *p >= '0' && *p <= '9')
            {
                ++p;
            }
            size_t digits = static_cast<size_t>(p - start);
            if (digits == 0 || digits > 6)
            {
                return std::nullopt;
            }
            for (auto const* q = start; q < p; ++q)
            {
                us = us * 10 + static_cast<unsigned long>(*q - '0');
            }
            while (digits < 6)
            {
                us *= 10;
                ++digits;
            }
        }
        if (p != end)
        {
            return std::nullopt;
        }

        time r { h, m, sec, us, neg };
        return r.is_valid() ? std::optional<time>(r) : std::nullopt;
    }

    constexpr std::strong_ordering
    time::operator<=>(time const& other) const noexcept
    {
        return to_duration() <=> other.to_duration();
    }

    constexpr bool
    time::operator==(time const& other) const noexcept
    {
        return to_duration() == other.to_duration();
    }

    constexpr bool
    datetime::is_valid() const noexcept
    {
        return date::is_valid() && time::is_valid() && !negative && hour < 24;
    }

    constexpr timestamp
    datetime::to_time_point() const
    {
        if (!is_valid())
        {
            throw std::runtime_error("db: invalid datetime value");
        }
        return date::to_sys_days() + std::chrono::hours { hour } + std::chrono::minutes { minute }
               + std::chrono::seconds { second } + std::chrono::microseconds { microsecond };
    }

    constexpr datetime
    datetime::from_time_point(timestamp tp) noexcept
    {
        auto days = std::chrono::floor<std::chrono::days>(tp);
        auto tod = tp - days;
        date d = date::from_sys_days(days);
        time t = time::from_duration(std::chrono::duration_cast<std::chrono::microseconds>(tod));
        return datetime { d.year, d.month, d.day, t.hour, t.minute, t.second, t.microsecond };
    }

    inline std::string
    datetime::to_string() const
    {
        std::string s = date::to_string();
        s += ' ';
        char buf[32];
        if (microsecond)
        {
            std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%06lu", hour, minute, second, microsecond);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hour, minute, second);
        }
        s += buf;
        return s;
    }

    inline std::optional<datetime>
    datetime::from_string(std::string_view sv)
    {
        auto sp = sv.find(' ');
        if (sp == std::string_view::npos)
        {
            return std::nullopt;
        }
        auto d = date::from_string(sv.substr(0, sp));
        auto t = time::from_string(sv.substr(sp + 1));
        if (!d || !t)
        {
            return std::nullopt;
        }
        datetime r { d->year, d->month, d->day, t->hour, t->minute, t->second, t->microsecond, t->negative };
        return r.is_valid() ? std::optional<datetime>(r) : std::nullopt;
    }

    constexpr std::chrono::microseconds
    operator-(datetime const& a, datetime const& b)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(a.to_time_point() - b.to_time_point());
    }

} // namespace httplib::db

namespace std
{
    template <>
    struct hash<httplib::db::date>
    {
        size_t
        operator()(httplib::db::date const& d) const noexcept
        {
            size_t h = std::hash<unsigned> {}(d.year);
            h = h * 31u + std::hash<unsigned> {}(d.month);
            h = h * 31u + std::hash<unsigned> {}(d.day);
            return h;
        }
    };

    template <>
    struct hash<httplib::db::time>
    {
        size_t
        operator()(httplib::db::time const& t) const noexcept
        {
            return std::hash<int64_t> {}(t.to_duration().count());
        }
    };

    template <>
    struct hash<httplib::db::datetime>
    {
        size_t
        operator()(httplib::db::datetime const& dt) const noexcept
        {
            size_t h = std::hash<httplib::db::date> {}(static_cast<httplib::db::date const&>(dt));
            h = h * 31u + std::hash<httplib::db::time> {}(static_cast<httplib::db::time const&>(dt));
            return h;
        }
    };
} // namespace std
