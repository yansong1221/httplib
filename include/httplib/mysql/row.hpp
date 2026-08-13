#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "mysql_fwd.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace httplib::mysql
{

    enum class column_type
    {
        string,
        int64,
        uint64,
        double_,
        blob,
        date,
        datetime,
        timestamp,
        time,
        null,
        unknown
    };
    struct date
    {
        unsigned year = 0, month = 0, day = 0;
    };
    struct time
    {
        unsigned hour = 0, minute = 0, second = 0;
        unsigned long microsecond = 0;
    };
    struct datetime
        : date
        , time
    {
    };

    class HTTPLIB_API row
    {
      public:
        row(row&&) noexcept;
        row& operator=(row&&) noexcept;
        ~row();

        size_t size() const;
        size_t column(std::string_view name) const;
        bool is_null(size_t col) const;
        bool is_null(std::string_view name) const;

        std::optional<std::string_view> as_string(size_t col) const;
        std::optional<std::string_view> as_string(std::string_view name) const;

        std::optional<int64_t> as_int64(size_t col) const;
        std::optional<int64_t> as_int64(std::string_view name) const;

        std::optional<uint64_t> as_uint64(size_t col) const;
        std::optional<uint64_t> as_uint64(std::string_view name) const;

        std::optional<double> as_double(size_t col) const;
        std::optional<double> as_double(std::string_view name) const;

        std::optional<float> as_float(size_t col) const;
        std::optional<float> as_float(std::string_view name) const;

        std::optional<bool> as_bool(size_t col) const;
        std::optional<bool> as_bool(std::string_view name) const;

        std::optional<net::const_buffer> as_blob(size_t col) const;
        std::optional<net::const_buffer> as_blob(std::string_view name) const;

        std::optional<boost::json::value> as_json(size_t col) const;
        std::optional<boost::json::value> as_json(std::string_view name) const;

        std::optional<date> as_date(size_t col) const;
        std::optional<date> as_date(std::string_view name) const;

        std::optional<datetime> as_datetime(size_t col) const;
        std::optional<datetime> as_datetime(std::string_view name) const;

        std::optional<time> as_time(size_t col) const;
        std::optional<time> as_time(std::string_view name) const;

        std::optional<std::chrono::system_clock::time_point> as_timestamp(size_t col) const;
        std::optional<std::chrono::system_clock::time_point> as_timestamp(std::string_view name) const;

        template <typename T>
        std::optional<T>
        get(size_t col) const
        {
            if constexpr (std::is_same_v<T, int64_t>)
            {
                return as_int64(col);
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                return as_uint64(col);
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
            else if constexpr (std::is_same_v<T, net::const_buffer>)
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
            else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>)
            {
                return as_timestamp(col);
            }
            else if constexpr (std::is_same_v<T, boost::json::value>)
            {
                return as_json(col);
            }
            else
            {
                static_assert(sizeof(T) == 0, "unsupported get<T> type");
            }
        }
        template <typename T>
        std::optional<T>
        get(std::string_view name) const
        {
            return get<T>(column(name));
        }

        struct impl;
        explicit row(std::unique_ptr<impl> p);

      private:
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::mysql
#endif