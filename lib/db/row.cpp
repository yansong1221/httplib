#include "httplib/db/row.hpp"
#include "httplib/db/exception.hpp"
#include "httplib/db/result.hpp"
#include <boost/json.hpp>
#include <limits>

namespace httplib::db
{
    namespace detail
    {
        /// 单类型字段直取：NULL → nullopt，类型不匹配 → 抛异常。
        template <typename T>
        std::optional<T>
        extract(field const& f, std::string_view what)
        {
            if (std::holds_alternative<std::monostate>(f))
            {
                return std::nullopt;
            }
            auto* v = std::get_if<T>(&f);
            if (!v)
            {
                throw db_exception(boost::system::error_code {}, "db: cannot convert to " + std::string(what));
            }
            return *v;
        }

        static std::optional<int64_t>
        as_int64(field const& f)
        {
            if (std::holds_alternative<std::monostate>(f))
            {
                return std::nullopt;
            }
            if (auto* v = std::get_if<int64_t>(&f))
            {
                return *v;
            }
            if (auto* v = std::get_if<uint64_t>(&f))
            {
                if (*v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                {
                    throw db_exception(boost::system::error_code {}, "db: value out of range for int64");
                }
                return static_cast<int64_t>(*v);
            }
            if (auto* v = std::get_if<double>(&f))
            {
                if (*v < static_cast<double>(std::numeric_limits<int64_t>::min())
                    || *v >= static_cast<double>(std::numeric_limits<int64_t>::max()))
                {
                    throw db_exception(boost::system::error_code {}, "db: value out of range for int64");
                }
                return static_cast<int64_t>(*v);
            }
            throw db_exception(boost::system::error_code {}, "db: cannot convert to int64");
        }

        static std::optional<uint64_t>
        as_uint64(field const& f)
        {
            if (std::holds_alternative<std::monostate>(f))
            {
                return std::nullopt;
            }
            if (auto* v = std::get_if<uint64_t>(&f))
            {
                return *v;
            }
            if (auto* v = std::get_if<int64_t>(&f))
            {
                if (*v < 0)
                {
                    throw db_exception(boost::system::error_code {}, "db: value out of range for uint64");
                }
                return static_cast<uint64_t>(*v);
            }
            if (auto* v = std::get_if<double>(&f))
            {
                if (*v < 0.0 || *v >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
                {
                    throw db_exception(boost::system::error_code {}, "db: value out of range for uint64");
                }
                return static_cast<uint64_t>(*v);
            }
            throw db_exception(boost::system::error_code {}, "db: cannot convert to uint64");
        }

        static std::optional<double>
        as_double(field const& f)
        {
            if (std::holds_alternative<std::monostate>(f))
            {
                return std::nullopt;
            }
            if (auto* v = std::get_if<double>(&f))
            {
                return *v;
            }
            if (auto* v = std::get_if<int64_t>(&f))
            {
                return static_cast<double>(*v);
            }
            if (auto* v = std::get_if<uint64_t>(&f))
            {
                return static_cast<double>(*v);
            }
            throw db_exception(boost::system::error_code {}, "db: cannot convert to double");
        }

        static std::optional<bool>
        as_bool(field const& f)
        {
            if (std::holds_alternative<std::monostate>(f))
            {
                return std::nullopt;
            }
            if (auto* v = std::get_if<int64_t>(&f))
            {
                return *v != 0;
            }
            if (auto* v = std::get_if<uint64_t>(&f))
            {
                return *v != 0;
            }
            if (auto* v = std::get_if<double>(&f))
            {
                return *v != 0.0;
            }
            throw db_exception(boost::system::error_code {}, "db: cannot convert to bool");
        }
    } // namespace detail

    row::row(result const* parent, size_t idx) noexcept : parent_(parent), idx_(idx) {}

    size_t
    row::size() const
    {
        return parent_->column_count();
    }

    size_t
    row::column(std::string_view name) const
    {
        return parent_->column_index(name);
    }

    bool
    row::is_null(size_t col) const
    {
        return std::holds_alternative<std::monostate>(parent_->at(idx_, col));
    }

    bool
    row::is_null(std::string_view name) const
    {
        return is_null(column(name));
    }

    std::optional<std::string_view>
    row::as_string(size_t col) const
    {
        auto t = as_text(col);
        return t ? std::optional<std::string_view>(t->data()) : std::nullopt;
    }

    std::optional<std::string_view>
    row::as_string(std::string_view name) const
    {
        return as_string(column(name));
    }

    std::optional<text>
    row::as_text(size_t col) const
    {
        return detail::extract<text>(parent_->at(idx_, col), "text");
    }

    std::optional<text>
    row::as_text(std::string_view name) const
    {
        return as_text(column(name));
    }

    std::optional<int64_t>
    row::as_int64(size_t col) const
    {
        return detail::as_int64(parent_->at(idx_, col));
    }

    std::optional<int64_t>
    row::as_int64(std::string_view name) const
    {
        return as_int64(column(name));
    }

    std::optional<uint64_t>
    row::as_uint64(size_t col) const
    {
        return detail::as_uint64(parent_->at(idx_, col));
    }

    std::optional<uint64_t>
    row::as_uint64(std::string_view name) const
    {
        return as_uint64(column(name));
    }

    std::optional<double>
    row::as_double(size_t col) const
    {
        return detail::as_double(parent_->at(idx_, col));
    }

    std::optional<double>
    row::as_double(std::string_view name) const
    {
        return as_double(column(name));
    }

    std::optional<float>
    row::as_float(size_t col) const
    {
        auto d = as_double(col);
        return d ? std::optional<float>(static_cast<float>(*d)) : std::nullopt;
    }

    std::optional<float>
    row::as_float(std::string_view name) const
    {
        return as_float(column(name));
    }

    std::optional<bool>
    row::as_bool(size_t col) const
    {
        return detail::as_bool(parent_->at(idx_, col));
    }

    std::optional<bool>
    row::as_bool(std::string_view name) const
    {
        return as_bool(column(name));
    }

    std::optional<std::span<std::byte const>>
    row::as_blob(size_t col) const
    {
        auto b = as_blob_value(col);
        return b ? std::optional<std::span<std::byte const>>(b->data()) : std::nullopt;
    }

    std::optional<std::span<std::byte const>>
    row::as_blob(std::string_view name) const
    {
        return as_blob(column(name));
    }

    std::optional<blob>
    row::as_blob_value(size_t col) const
    {
        return detail::extract<blob>(parent_->at(idx_, col), "blob");
    }

    std::optional<blob>
    row::as_blob_value(std::string_view name) const
    {
        return as_blob_value(column(name));
    }

    std::optional<boost::json::value>
    row::as_json(size_t col) const
    {
        auto sv = as_string(col);
        if (!sv)
        {
            return std::nullopt;
        }
        boost::system::error_code ec;
        boost::json::value jv = boost::json::parse(*sv, ec);
        if (ec)
        {
            throw db_exception(boost::system::error_code {}, "db: json parse error: " + ec.message());
        }
        return jv;
    }

    std::optional<boost::json::value>
    row::as_json(std::string_view name) const
    {
        return as_json(column(name));
    }

    std::optional<date>
    row::as_date(size_t col) const
    {
        return detail::extract<date>(parent_->at(idx_, col), "date");
    }

    std::optional<date>
    row::as_date(std::string_view name) const
    {
        return as_date(column(name));
    }

    std::optional<datetime>
    row::as_datetime(size_t col) const
    {
        auto& f = parent_->at(idx_, col);
        if (std::holds_alternative<std::monostate>(f))
        {
            return std::nullopt;
        }
        if (auto* v = std::get_if<datetime>(&f))
        {
            return *v;
        }
        if (auto* v = std::get_if<timestamp>(&f))
        {
            return datetime::from_time_point(*v);
        }
        throw db_exception(boost::system::error_code {}, "db: cannot convert to datetime");
    }

    std::optional<datetime>
    row::as_datetime(std::string_view name) const
    {
        return as_datetime(column(name));
    }

    std::optional<time>
    row::as_time(size_t col) const
    {
        return detail::extract<time>(parent_->at(idx_, col), "time");
    }

    std::optional<time>
    row::as_time(std::string_view name) const
    {
        return as_time(column(name));
    }

    std::optional<timestamp>
    row::as_timestamp(size_t col) const
    {
        auto& f = parent_->at(idx_, col);
        if (std::holds_alternative<std::monostate>(f))
        {
            return std::nullopt;
        }
        if (auto* v = std::get_if<timestamp>(&f))
        {
            return *v;
        }
        if (auto* v = std::get_if<datetime>(&f))
        {
            // DATETIME 列读回的是无时区墙上时钟，直接视为 UTC 时间点。
            return v->to_time_point();
        }
        throw db_exception(boost::system::error_code {}, "db: cannot convert to timestamp");
    }

    std::optional<timestamp>
    row::as_timestamp(std::string_view name) const
    {
        return as_timestamp(column(name));
    }

} // namespace httplib::db
