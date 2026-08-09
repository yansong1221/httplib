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
        time,
        null,
        unknown
    };
    struct date
    {
        unsigned year = 0, month = 0, day = 0;
    };
    struct datetime
    {
        unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        unsigned long microsecond = 0;
    };

    class HTTPLIB_API row
    {
      public:
        row() = delete;
        row(row const&) = delete;
        row& operator=(row const&) = delete;
        row(row&&) noexcept;
        row& operator=(row&&) noexcept;
        ~row();

        std::string operator[](size_t col) const;
        std::string operator[](std::string_view name) const;
        size_t size() const;
        size_t column(std::string_view name) const;
        bool is_null(size_t col) const;
        bool is_null(std::string_view name) const;

        std::string as_string(size_t col, std::optional<std::string_view> d = {}) const;
        std::string as_string(std::string_view name, std::optional<std::string_view> d = {}) const;

        int64_t as_int64(size_t col, std::optional<int64_t> d = {}) const;
        int64_t as_int64(std::string_view name, std::optional<int64_t> d = {}) const;

        uint64_t as_uint64(size_t col, std::optional<uint64_t> d = {}) const;
        uint64_t as_uint64(std::string_view name, std::optional<uint64_t> d = {}) const;

        double as_double(size_t col, std::optional<double> d = {}) const;
        double as_double(std::string_view name, std::optional<double> d = {}) const;

        float as_float(size_t col, std::optional<float> d = {}) const;
        float as_float(std::string_view name, std::optional<float> d = {}) const;

        bool as_bool(size_t col, std::optional<bool> d = {}) const;
        bool as_bool(std::string_view name, std::optional<bool> d = {}) const;

        net::const_buffer as_blob(size_t col) const;
        net::const_buffer as_blob(std::string_view name) const;

        date as_date(size_t col) const;
        date as_date(std::string_view name) const;

        datetime as_datetime(size_t col) const;
        datetime as_datetime(std::string_view name) const;

        std::chrono::steady_clock::duration as_duration(size_t col,
                                                          std::optional<std::chrono::steady_clock::duration> d
                                                          = {}) const;
        std::chrono::steady_clock::duration as_duration(std::string_view name,
                                                         std::optional<std::chrono::steady_clock::duration> d
                                                         = {}) const;

        std::chrono::system_clock::time_point as_timestamp(size_t col,
                                                            std::optional<std::chrono::system_clock::time_point> d
                                                            = {}) const;
        std::chrono::system_clock::time_point as_timestamp(std::string_view name,
                                                            std::optional<std::chrono::system_clock::time_point> d
                                                            = {}) const;

        template <typename T>
        T
        get(size_t col, std::optional<T> d = {}) const
        {
            if constexpr (std::is_same_v<T, int64_t>)
            {
                return as_int64(col, d);
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                return as_uint64(col, d);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return as_double(col, d);
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                return as_float(col, d);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return as_bool(col, d);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return as_string(col, d);
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
            else if constexpr (std::is_same_v<T, std::chrono::steady_clock::duration>)
            {
                return as_duration(col, d);
            }
            else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>)
            {
                return as_timestamp(col, d);
            }
            else
            {
                static_assert(sizeof(T) == 0, "unsupported get<T> type");
            }
        }
        template <typename T>
        T
        get(std::string_view name, std::optional<T> d = {}) const
        {
            return get<T>(column(name), d);
        }

        struct impl;
        explicit row(std::unique_ptr<impl> p);

      private:
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::mysql
#endif