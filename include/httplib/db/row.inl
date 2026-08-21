#pragma once
#include "exception.hpp"
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace httplib::db
{

    namespace detail
    {
        /// 把 int64 窄化到更小的整数类型，越界抛异常。
        template <typename T>
        std::optional<T>
        narrow_int(std::optional<int64_t> v)
        {
            if (!v)
            {
                return std::nullopt;
            }
            if (*v < static_cast<int64_t>(std::numeric_limits<T>::min())
                || *v > static_cast<int64_t>(std::numeric_limits<T>::max()))
            {
                throw db_exception(boost::system::error_code {}, "db: value out of range for integer type");
            }
            return static_cast<T>(*v);
        }

        template <typename T>
        std::optional<T>
        narrow_uint(std::optional<uint64_t> v)
        {
            if (!v)
            {
                return std::nullopt;
            }
            if (*v > static_cast<uint64_t>(std::numeric_limits<T>::max()))
            {
                throw db_exception(boost::system::error_code {}, "db: value out of range for integer type");
            }
            return static_cast<T>(*v);
        }
    } // namespace detail

    template <typename T>
    std::optional<T>
    row::get(size_t col) const
    {
        if constexpr (std::is_same_v<T, int64_t>)
        {
            return as_int64(col);
        }
        else if constexpr (std::is_same_v<T, uint64_t>)
        {
            return as_uint64(col);
        }
        else if constexpr (std::is_same_v<T, int>)
        {
            return detail::narrow_int<int>(as_int64(col));
        }
        else if constexpr (std::is_same_v<T, unsigned>)
        {
            return detail::narrow_uint<unsigned>(as_uint64(col));
        }
        else if constexpr (std::is_same_v<T, short>)
        {
            return detail::narrow_int<short>(as_int64(col));
        }
        else if constexpr (std::is_same_v<T, unsigned short>)
        {
            return detail::narrow_uint<unsigned short>(as_uint64(col));
        }
        else if constexpr (std::is_same_v<T, double>)
        {
            return as_double(col);
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            return as_float(col);
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            return as_bool(col);
        }
        else if constexpr (std::is_same_v<T, std::string_view>)
        {
            return as_string(col);
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            auto sv = as_string(col);
            return sv ? std::optional<std::string>(std::string(*sv)) : std::nullopt;
        }
        else if constexpr (std::is_same_v<T, std::span<std::byte const>>)
        {
            return as_blob(col);
        }
        else if constexpr (std::is_same_v<T, date>)
        {
            return as_date(col);
        }
        else if constexpr (std::is_same_v<T, datetime>)
        {
            return as_datetime(col);
        }
        else if constexpr (std::is_same_v<T, time>)
        {
            return as_time(col);
        }
        else if constexpr (std::is_same_v<T, timestamp>)
        {
            return as_timestamp(col);
        }
        else if constexpr (std::is_same_v<T, boost::json::value>)
        {
            return as_json(col);
        }
        else
        {
            static_assert(sizeof(T) == 0, "db: unsupported get<T> type");
        }
    }

    template <typename T>
    std::optional<T>
    row::get(std::string_view name) const
    {
        return get<T>(column(name));
    }

} // namespace httplib::db
