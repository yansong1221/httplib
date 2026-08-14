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

    /**
     * \brief 结果集列的类型。
     */
    enum class column_type
    {
        string,    ///< 字符串（CHAR/VARCHAR/TEXT/JSON 等）。
        int64,     ///< 有符号整数。
        uint64,    ///< 无符号整数。
        double_,   ///< 浮点数。
        blob,      ///< 二进制数据。
        date,      ///< DATE。
        datetime,  ///< DATETIME。
        timestamp, ///< TIMESTAMP（时区敏感）。
        time,      ///< TIME。
        null,      ///< NULL。
        unknown    ///< 未知类型。
    };

    /**
     * \brief 日期（年/月/日）。
     */
    struct date
    {
        unsigned year = 0, month = 0, day = 0;
    };

    /**
     * \brief 时间（时/分/秒/微秒）。
     */
    struct time
    {
        unsigned hour = 0, minute = 0, second = 0;
        unsigned long microsecond = 0;
    };

    /**
     * \brief 日期时间（date 与 time 的组合）。
     */
    struct datetime
        : date
        , time
    {
    };

    /**
     * \brief 结果集中的一行。
     * \details
     * 通过 \ref result::operator[] 或 \ref result::iterator 获取。
     * \n
     * 除 `get<std::string>` 外的 `as_*` 系列方法返回的视图（string_view / const_buffer）指向结果集内部缓冲区，
     * 在 result 被销毁后会失效；`get<std::string>` 会拷贝一份。
     */
    class HTTPLIB_API row
    {
      public:
        row(row&&) noexcept;
        row& operator=(row&&) noexcept;
        ~row();

        /**
         * \brief 该行的列数。
         */
        size_t size() const;
        /**
         * \brief 按列名查找列下标，找不到抛异常。
         */
        size_t column(std::string_view name) const;
        /**
         * \brief 判断指定列是否为 NULL。
         */
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

        /**
         * \brief 按模板参数类型读取列值。
         * \details
         * 支持 int64_t / uint64_t / double / float / bool / std::string / std::string_view /
         * net::const_buffer / date / datetime / time / system_clock::time_point / boost::json::value。
         * \param col 列下标。
         * \returns 列值；NULL 返回 nullopt。
         */
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

        /**
         * \brief 按模板参数类型、列名读取列值。
         * \param name 列名。
         */
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
#endif // HTTPLIB_ENABLED_DATABASE
