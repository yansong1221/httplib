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
        static boost::mysql::field_view
        ff(result::impl const& i, size_t r, size_t c)
        {
            return i.rows().at(r).at(c);
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

        template <typename T, typename Conv>
        static std::optional<T>
        detail_value(row::impl const& imp, size_t col, Conv&& conv)
        {
            auto f = detail::ff(get_impl(*imp.parent), imp.idx, col);
            if (f.is_null())
            {
                return std::nullopt;
            }
            return conv(f);
        }
    } // namespace detail

    row::row(std::unique_ptr<impl> p) : impl_(std::move(p)) {}

    row::row(row&&) noexcept = default;
    row& row::operator=(row&&) noexcept = default;

    row::~row() = default;

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

    std::optional<std::string_view>
    row::as_string(size_t col) const
    {
        return detail::detail_value<std::string_view>(*impl_,
                                                       col,
                                                       [this](auto& f) -> std::string_view
                                                       {
                                                           if (!f.is_string())
                                                           {
                                                               throw std::runtime_error("db: cannot convert to string");
                                                           }
                                                           return f.as_string();
                                                       });
    }
    std::optional<std::string_view>
    row::as_string(std::string_view name) const
    {
        return as_string(column(name));
    }

    std::optional<int64_t>
    row::as_int64(size_t col) const
    {
        return detail::detail_value<int64_t>(*impl_, col, detail::fi);
    }
    std::optional<int64_t>
    row::as_int64(std::string_view name) const
    {
        return as_int64(column(name));
    }

    std::optional<uint64_t>
    row::as_uint64(size_t col) const
    {
        return detail::detail_value<uint64_t>(*impl_, col, detail::fu);
    }
    std::optional<uint64_t>
    row::as_uint64(std::string_view name) const
    {
        return as_uint64(column(name));
    }

    std::optional<double>
    row::as_double(size_t col) const
    {
        return detail::detail_value<double>(*impl_, col, detail::fd);
    }
    std::optional<double>
    row::as_double(std::string_view name) const
    {
        return as_double(column(name));
    }

    std::optional<float>
    row::as_float(size_t col) const
    {
        return detail::detail_value<float>(*impl_, col, [](auto& f) { return (float)detail::fd(f); });
    }
    std::optional<float>
    row::as_float(std::string_view name) const
    {
        return as_float(column(name));
    }

    std::optional<bool>
    row::as_bool(size_t col) const
    {
        auto v = detail::detail_value<int64_t>(*impl_, col, detail::fi);
        return v.has_value() ? std::optional<bool>(*v != 0) : std::nullopt;
    }
    std::optional<bool>
    row::as_bool(std::string_view name) const
    {
        return as_bool(column(name));
    }

    std::optional<net::const_buffer>
    row::as_blob(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            return std::nullopt;
        }
        if (!f.is_blob())
        {
            throw std::runtime_error("db: cannot convert to blob");
        }
        auto b = f.as_blob();
        return net::const_buffer(b.data(), b.size());
    }
    std::optional<net::const_buffer>
    row::as_blob(std::string_view name) const
    {
        return as_blob(column(name));
    }

    std::optional<date>
    row::as_date(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            return std::nullopt;
        }
        if (!f.is_date())
        {
            throw std::runtime_error("db: cannot convert to date");
        }
        auto d = f.as_date();
        return date { d.year(), d.month(), d.day() };
    }
    std::optional<date>
    row::as_date(std::string_view name) const
    {
        return as_date(column(name));
    }

    std::optional<datetime>
    row::as_datetime(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            return std::nullopt;
        }
        if (!f.is_datetime())
        {
            throw std::runtime_error("db: cannot convert to datetime");
        }
        auto d = f.as_datetime();
        return datetime { d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second(), d.microsecond() };
    }
    std::optional<datetime>
    row::as_datetime(std::string_view name) const
    {
        return as_datetime(column(name));
    }

    std::optional<time>
    row::as_time(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            return std::nullopt;
        }
        if (!f.is_time())
        {
            throw std::runtime_error("db: cannot convert to time");
        }
        auto t = f.as_time();
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t);
        auto s = std::chrono::duration_cast<std::chrono::seconds>(dur).count();
        return time { (unsigned)(s / 3600),
                      (unsigned)((s % 3600) / 60),
                      (unsigned)(s % 60),
                      (unsigned long)(dur.count() % 1000000) };
    }
    std::optional<time>
    row::as_time(std::string_view name) const
    {
        return as_time(column(name));
    }

    std::optional<boost::json::value>
    row::as_json(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            return std::nullopt;
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
    std::optional<boost::json::value>
    row::as_json(std::string_view name) const
    {
        return as_json(column(name));
    }

    std::optional<std::chrono::system_clock::time_point>
    row::as_timestamp(size_t col) const
    {
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            return std::nullopt;
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
    std::optional<std::chrono::system_clock::time_point>
    row::as_timestamp(std::string_view name) const
    {
        return as_timestamp(column(name));
    }

    size_t
    row::impl::col_of(std::string_view name) const
    {
        return parent->column_index(name);
    }

    void
    result::impl::load_resultset(size_t idx)
    {
        auto rs = data[idx];
        affected = rs.affected_rows();
        insert_id = rs.last_insert_id();
        warnings = rs.warning_count();
        col_names.clear();
        col_types.clear();
        if (rs.has_value())
        {
            auto m = rs.meta();
            col_names.reserve(m.size());
            col_types.reserve(m.size());
            for (auto& c : m)
            {
                auto s = c.column_name();
                col_names.emplace_back(s.data(), s.size());
                col_types.push_back(detail::map_column_type(c.type(), c.is_unsigned()));
            }
        }
    }

    result::impl::impl(boost::mysql::results&& r) : data(std::move(r))
    {
        if (data.has_value())
        {
            load_resultset(0);
        }
    }

    boost::mysql::rows_view
    result::impl::rows() const
    {
        return data[resultset_index].rows();
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
        return !i.data.has_value() || i.rows().empty();
    }
    size_t
    result::resultset_count() const
    {
        auto& i = get_impl(*this);
        return i.data.has_value() ? i.data.size() : 0;
    }
    bool
    result::next_resultset()
    {
        auto& i = get_impl(*this);
        if (i.resultset_index + 1 >= i.data.size())
        {
            return false;
        }
        ++i.resultset_index;
        i.load_resultset(i.resultset_index);
        return true;
    }
    size_t
    result::row_count() const
    {
        auto& i = get_impl(*this);
        return i.data.has_value() ? i.rows().size() : 0;
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
