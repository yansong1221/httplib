#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/result.hpp"
#include "mysql/result_impl.h"
#include <boost/mysql.hpp>
#include <chrono>
#include <limits>

namespace httplib::mysql::detail
{

    inline column_type
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
                return column_type::datetime;
            case b::timestamp:
                return column_type::timestamp;
            case b::time:
                return column_type::time;
            default:
                return column_type::unknown;
        }
    }

    inline boost::mysql::field_view
    ff(result::impl const& i, size_t r, size_t c)
    {
        return i.rows().at(r).at(c);
    }

    inline int64_t
    fi(boost::mysql::field_view const& f)
    {
        if (f.is_int64())
        {
            return f.as_int64();
        }
        if (f.is_uint64())
        {
            auto v = f.as_uint64();
            if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            {
                throw std::runtime_error("db: value out of range for int64");
            }
            return static_cast<int64_t>(v);
        }
        if (f.is_double())
        {
            auto v = f.as_double();
            if (v < static_cast<double>(std::numeric_limits<int64_t>::min())
                || v >= static_cast<double>(std::numeric_limits<int64_t>::max()))
            {
                throw std::runtime_error("db: value out of range for int64");
            }
            return static_cast<int64_t>(v);
        }
        throw std::runtime_error("db: cannot convert to int64");
    }

    inline uint64_t
    fu(boost::mysql::field_view const& f)
    {
        if (f.is_uint64())
        {
            return f.as_uint64();
        }
        if (f.is_int64())
        {
            auto v = f.as_int64();
            if (v < 0)
            {
                throw std::runtime_error("db: value out of range for uint64");
            }
            return static_cast<uint64_t>(v);
        }
        if (f.is_double())
        {
            auto v = f.as_double();
            if (v < 0.0 || v >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
            {
                throw std::runtime_error("db: value out of range for uint64");
            }
            return static_cast<uint64_t>(v);
        }
        throw std::runtime_error("db: cannot convert to uint64");
    }

    inline double
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

    inline std::chrono::system_clock::time_point
    d2e(datetime const& dt)
    {
        return dt.to_time_point();
    }

    template <typename T, typename Conv>
    inline std::optional<T>
    detail_value(result::impl const& i, size_t idx, size_t col, Conv&& conv)
    {
        auto f = ff(i, idx, col);
        if (f.is_null())
        {
            return std::nullopt;
        }
        return conv(f);
    }

} // namespace httplib::mysql::detail
#endif
