#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/row.hpp"
#include "mysql/detail_helpers.h"
#include "mysql/result_impl.h"
#include "mysql/row_impl.h"
#include <boost/json.hpp>

namespace httplib::mysql
{

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
        auto f = detail::ff(get_impl(*impl_->parent), impl_->idx, col);
        if (f.is_null())
        {
            return std::nullopt;
        }
        if (f.is_int64())
        {
            return f.as_int64() != 0;
        }
        if (f.is_uint64())
        {
            return f.as_uint64() != 0;
        }
        if (f.is_double())
        {
            return f.as_double() != 0.0;
        }
        throw std::runtime_error("db: cannot convert to bool");
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
        if (dur.count() < 0)
        {
            throw std::runtime_error("db: negative TIME not supported");
        }
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
        auto d = f.as_datetime();
        auto tp = detail::d2e({ d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second(), d.microsecond() });
        auto const& parent = get_impl(*impl_->parent);
        if (col < parent.col_types.size() && parent.col_types[col] == column_type::timestamp)
        {
            tp -= parent.utc_offset;
        }
        return tp;
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

} // namespace httplib::mysql
#endif
