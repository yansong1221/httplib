#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/result.hpp"
#include "mysql/result_impl.h"
#include "mysql/row_impl.h"
#include <charconv>

namespace httplib::mysql
{
    namespace detail
    {
        static column_type
        map_column_type(boost::mysql::column_type t, bool u)
        {
            using b = boost::mysql::column_type;
            switch (t)
            {
                case b::tinyint:
                case b::smallint:
                case b::mediumint:
                case b::int_:
                case b::bigint:
                case b::year:
                    return u ? column_type::uint64 : column_type::int64;
                case b::bit:
                    return column_type::uint64;
                case b::float_:
                case b::double_:
                case b::decimal:
                    return column_type::double_;
                case b::varchar:
                case b::char_:
                case b::text:
                case b::enum_:
                case b::set:
                case b::json:
                    return column_type::string;
                case b::blob:
                case b::geometry:
                    return column_type::blob;
                case b::date:
                    return column_type::date;
                case b::datetime:
                case b::timestamp:
                    return column_type::datetime;
                case b::time:
                    return column_type::time;
                default:
                    return column_type::unknown;
            }
        }
        static std::string
        fs(boost::mysql::field_view const& f)
        {
            if (f.is_null())
            {
                return {};
            }
            if (f.is_string())
            {
                return std::string(f.as_string());
            }
            if (f.is_int64())
            {
                char b[24];
                auto [p, _] = std::to_chars(b, b + 24, f.as_int64());
                return { b, p };
            }
            if (f.is_uint64())
            {
                char b[24];
                auto [p, _] = std::to_chars(b, b + 24, f.as_uint64());
                return { b, p };
            }
            if (f.is_double())
            {
                char b[32];
                auto [p, _] = std::to_chars(b, b + 32, f.as_double());
                return { b, p };
            }
            if (f.is_blob())
            {
                auto x = f.as_blob();
                return { reinterpret_cast<char const*>(x.data()), x.size() };
            }
            if (f.is_date())
            {
                auto d = f.as_date();
                char b[16];
                snprintf(b, 16, "%04u-%02u-%02u", d.year(), d.month(), d.day());
                return b;
            }
            if (f.is_datetime())
            {
                auto d = f.as_datetime();
                char b[32];
                snprintf(b,
                         32,
                         "%04u-%02u-%02u %02u:%02u:%02u",
                         d.year(),
                         d.month(),
                         d.day(),
                         d.hour(),
                         d.minute(),
                         d.second());
                return b;
            }
            if (f.is_time())
            {
                auto t = f.as_time();
                auto s = std::chrono::duration_cast<std::chrono::seconds>(t).count();
                bool n = s < 0;
                if (n)
                {
                    s = -s;
                }
                auto h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
                char b[24];
                if (n)
                {
                    snprintf(b, 24, "-%02lld:%02lld:%02lld", h, m, sec);
                }
                else
                {
                    snprintf(b, 24, "%02lld:%02lld:%02lld", h, m, sec);
                }
                return b;
            }
            return {};
        }
        static boost::mysql::field_view
        ff(result::impl const& i, size_t r, size_t c)
        {
            return i.data.rows().at(r).at(c);
        }
        static int64_t
        fi(boost::mysql::field_view const& f)
        {
            if (f.is_int64())
            {
                return f.as_int64();
            }
            if (f.is_uint64())
            {
                return (int64_t)f.as_uint64();
            }
            if (f.is_double())
            {
                return (int64_t)f.as_double();
            }
            throw std::runtime_error("db: cannot convert to int64");
        }
        static uint64_t
        fu(boost::mysql::field_view const& f)
        {
            if (f.is_uint64())
            {
                return f.as_uint64();
            }
            if (f.is_int64())
            {
                return (uint64_t)f.as_int64();
            }
            if (f.is_double())
            {
                return (uint64_t)f.as_double();
            }
            throw std::runtime_error("db: cannot convert to uint64");
        }
        static double
        fd(boost::mysql::field_view const& f)
        {
            if (f.is_double())
            {
                return f.as_double();
            }
            if (f.is_int64())
            {
                return (double)f.as_int64();
            }
            if (f.is_uint64())
            {
                return (double)f.as_uint64();
            }
            throw std::runtime_error("db: cannot convert to double");
        }
        static std::chrono::system_clock::time_point
        d2e(datetime const& dt)
        {
            auto ymd = std::chrono::year(dt.year) / std::chrono::month(dt.month) / std::chrono::day(dt.day);
            auto tp = std::chrono::sys_days(ymd) + std::chrono::hours(dt.hour) + std::chrono::minutes(dt.minute)
                      + std::chrono::seconds(dt.second) + std::chrono::microseconds(dt.microsecond);
            return tp;
        }

        static void
        throw_null_error(row::impl const& imp, size_t col)
        {
            throw std::runtime_error("db: NULL in column '" + imp.parent->column_name(col) + "'");
        }

        template <typename T, typename Conv>
        static T
        field_cast(row::impl const& imp, size_t col, std::optional<T> const& d, Conv&& conv)
        {
            auto f = ff(get_impl(*imp.parent), imp.idx, col);
            if (f.is_null())
            {
                if (d)
                {
                    return *d;
                }
                throw_null_error(imp, col);
            }
            return conv(f);
        }
    } // namespace detail

    row::row(std::unique_ptr<impl> p) : impl_(std::move(p)) {}
    row::row(row&&) noexcept = default;
    row& row::operator=(row&&) noexcept = default;
    row::~row() = default;

    std::string
    row::operator[](size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            detail::throw_null_error(*impl_, col);
        }
        return detail::fs(f);
    }
    std::string
    row::operator[](std::string_view name) const
    {
        return (*this)[column(name)];
    }
    size_t
    row::size() const
    {
        return impl_->parent->column_count();
    }
    size_t
    row::column(std::string_view name) const
    {
        return impl_->col_of(name);
    }
    bool
    row::is_null(size_t col) const
    {
        return detail::ff(get_impl(*impl_->parent), impl_->idx, col).is_null();
    }
    bool
    row::is_null(std::string_view name) const
    {
        return is_null(column(name));
    }

    std::string
    row::as_string(size_t col, std::optional<std::string_view> d) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            if (d)
            {
                return std::string(*d);
            }
            detail::throw_null_error(*impl_, col);
        }
        return detail::fs(f);
    }
    std::string
    row::as_string(std::string_view name, std::optional<std::string_view> d) const
    {
        return as_string(column(name), d);
    }

    int64_t
    row::as_int64(size_t col, std::optional<int64_t> d) const
    {
        return detail::field_cast(*impl_, col, d, detail::fi);
    }
    int64_t
    row::as_int64(std::string_view name, std::optional<int64_t> d) const
    {
        return as_int64(column(name), d);
    }

    uint64_t
    row::as_uint64(size_t col, std::optional<uint64_t> d) const
    {
        return detail::field_cast(*impl_, col, d, detail::fu);
    }
    uint64_t
    row::as_uint64(std::string_view name, std::optional<uint64_t> d) const
    {
        return as_uint64(column(name), d);
    }

    double
    row::as_double(size_t col, std::optional<double> d) const
    {
        return detail::field_cast(*impl_, col, d, detail::fd);
    }
    double
    row::as_double(std::string_view name, std::optional<double> d) const
    {
        return as_double(column(name), d);
    }

    float
    row::as_float(size_t col, std::optional<float> d) const
    {
        return detail::field_cast<float>(*impl_, col, d, [](auto& f) { return (float)detail::fd(f); });
    }
    float
    row::as_float(std::string_view name, std::optional<float> d) const
    {
        return as_float(column(name), d);
    }

    bool
    row::as_bool(size_t col, std::optional<bool> d) const
    {
        return detail::field_cast(*impl_, col, d, detail::fi) != 0;
    }
    bool
    row::as_bool(std::string_view name, std::optional<bool> d) const
    {
        return as_bool(column(name), d);
    }

    net::const_buffer
    row::as_blob(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            detail::throw_null_error(*impl_, col);
        }
        if (!f.is_blob())
        {
            throw std::runtime_error("db: cannot convert to blob");
        }
        auto b = f.as_blob();
        return net::const_buffer(b.data(), b.size());
    }
    net::const_buffer
    row::as_blob(std::string_view name) const
    {
        return as_blob(column(name));
    }

    date
    row::as_date(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            detail::throw_null_error(*impl_, col);
        }
        if (!f.is_date())
        {
            throw std::runtime_error("db: cannot convert to date");
        }
        auto d = f.as_date();
        return { d.year(), d.month(), d.day() };
    }
    date
    row::as_date(std::string_view name) const
    {
        return as_date(column(name));
    }

    datetime
    row::as_datetime(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            detail::throw_null_error(*impl_, col);
        }
        if (!f.is_datetime())
        {
            throw std::runtime_error("db: cannot convert to datetime");
        }
        auto d = f.as_datetime();
        return { d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second(), d.microsecond() };
    }
    datetime
    row::as_datetime(std::string_view name) const
    {
        return as_datetime(column(name));
    }

    time
    row::as_time(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            detail::throw_null_error(*impl_, col);
        }
        if (!f.is_time())
        {
            throw std::runtime_error("db: cannot convert to time");
        }
        auto t = f.as_time();
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t);
        auto s = std::chrono::duration_cast<std::chrono::seconds>(dur).count();
        return { (unsigned)(s / 3600),
                 (unsigned)((s % 3600) / 60),
                 (unsigned)(s % 60),
                 (unsigned long)(dur.count() % 1000000) };
    }
    time
    row::as_time(std::string_view name) const
    {
        return as_time(column(name));
    }

    boost::json::value
    row::as_json(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            detail::throw_null_error(*impl_, col);
        }
        if (!f.is_string())
        {
            throw std::runtime_error("db: cannot convert to json");
        }
        boost::system::error_code ec;
        boost::json::value jv = boost::json::parse(f.as_string(), ec);
        if (ec)
        {
            throw std::runtime_error("db: json parse error: " + ec.message());
        }
        return jv;
    }
    boost::json::value
    row::as_json(std::string_view name) const
    {
        return as_json(column(name));
    }

    std::chrono::system_clock::time_point
    row::as_timestamp(size_t col, std::optional<std::chrono::system_clock::time_point> d) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            if (d)
            {
                return *d;
            }
            detail::throw_null_error(*impl_, col);
        }
        if (!f.is_datetime())
        {
            throw std::runtime_error("db: cannot convert to timestamp");
        }
        return detail::d2e({ f.as_datetime().year(),
                             f.as_datetime().month(),
                             f.as_datetime().day(),
                             f.as_datetime().hour(),
                             f.as_datetime().minute(),
                             f.as_datetime().second(),
                             f.as_datetime().microsecond() });
    }
    std::chrono::system_clock::time_point
    row::as_timestamp(std::string_view name, std::optional<std::chrono::system_clock::time_point> d) const
    {
        return as_timestamp(column(name), d);
    }

    size_t
    row::impl::col_of(std::string_view name) const
    {
        return parent->column_index(name);
    }

    result::impl::impl(boost::mysql::results&& r) : data(std::move(r))
    {
        if (!data.has_value())
        {
            return;
        }
        affected = data.affected_rows();
        insert_id = data.last_insert_id();
        warnings = data.warning_count();
        auto m = data.meta();
        col_names.reserve(m.size());
        col_types.reserve(m.size());
        for (auto& c : m)
        {
            auto s = c.column_name();
            col_names.emplace_back(s.data(), s.size());
            col_types.push_back(detail::map_column_type(c.type(), c.is_unsigned()));
        }
    }

    result::result() : impl_(std::make_unique<impl>()) {}
    result::result(std::unique_ptr<impl> p) : impl_(std::move(p)) {}
    result::result(result&&) noexcept = default;
    result& result::operator=(result&&) noexcept = default;
    result::~result() = default;
    result::impl&
    get_impl(result& s)
    {
        return *s.impl_;
    };
    result::impl const&
    get_impl(result const& s)
    {
        return *s.impl_;
    }
    bool
    result::empty() const
    {
        auto& i = get_impl(*this);
        return !i.data.has_value() || i.data.rows().empty();
    }
    size_t
    result::row_count() const
    {
        auto& i = get_impl(*this);
        return i.data.has_value() ? i.data.rows().size() : 0;
    }
    uint64_t
    result::affected_rows() const
    {
        return get_impl(*this).affected;
    }
    uint64_t
    result::last_insert_id() const
    {
        return get_impl(*this).insert_id;
    }
    uint64_t
    result::warning_count() const
    {
        return get_impl(*this).warnings;
    }
    size_t
    result::column_count() const
    {
        return get_impl(*this).col_names.size();
    }
    size_t
    result::column_index(std::string_view n) const
    {
        auto& i = get_impl(*this);
        for (size_t j = 0; j < i.col_names.size(); ++j)
        {
            if (i.col_names[j] == n)
            {
                return j;
            }
        }
        throw std::runtime_error("db: column not found: " + std::string(n));
    }
    std::string const&
    result::column_name(size_t c) const
    {
        return get_impl(*this).col_names[c];
    }
    column_type
    result::column_type(size_t c) const
    {
        return get_impl(*this).col_types[c];
    }
    row
    result::operator[](size_t i) const
    {
        auto imp = std::make_unique<row::impl>();
        imp->parent = this;
        imp->idx = i;
        return row(std::move(imp));
    }
    result::iterator
    result::begin() const
    {
        return iterator(this, 0);
    }
    result::iterator
    result::end() const
    {
        return iterator(this, row_count());
    }
} // namespace httplib::mysql
#endif
