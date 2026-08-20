#pragma once

#include "field.hpp"
#include "fwd.hpp"
#include "httplib/config.hpp"
#include <boost/json/value.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace httplib::db
{

    /**
     * \brief 结果集中的一行。
     * \details
     * 通过 \ref result::operator[] 或 \ref result::iterator 获取。
     * \n
     * 行内列值均为拥有型（存于 result 内部），不持有后端 buffer 的引用；
     * 但行本身是 result 的视图，result 销毁后行即失效。
     */
    class HTTPLIB_API row
    {
      public:
        /// 该行的列数。
        size_t size() const;
        /// 按列名查找列下标，找不到抛异常。
        size_t column(std::string_view name) const;
        /// 判断指定列是否为 NULL。
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

        std::optional<std::span<std::byte const>> as_blob(size_t col) const;
        std::optional<std::span<std::byte const>> as_blob(std::string_view name) const;

        std::optional<boost::json::value> as_json(size_t col) const;
        std::optional<boost::json::value> as_json(std::string_view name) const;

        std::optional<date> as_date(size_t col) const;
        std::optional<date> as_date(std::string_view name) const;

        std::optional<datetime> as_datetime(size_t col) const;
        std::optional<datetime> as_datetime(std::string_view name) const;

        std::optional<time> as_time(size_t col) const;
        std::optional<time> as_time(std::string_view name) const;

        std::optional<timestamp> as_timestamp(size_t col) const;
        std::optional<timestamp> as_timestamp(std::string_view name) const;

        /**
         * \brief 按模板参数类型读取列值。
         * \details
         * 支持 int64_t / uint64_t / int / unsigned / short / unsigned short / double / float / bool /
         * std::string / std::string_view / std::span<const std::byte> / date / datetime / time /
         * timestamp / boost::json::value。
         * \param col 列下标。
         * \returns 列值；NULL 返回 nullopt。
         */
        template <typename T>
        std::optional<T> get(size_t col) const;

        template <typename T>
        std::optional<T> get(std::string_view name) const;

      private:
        row(result const* parent, size_t idx) noexcept;

        result const* parent_ = nullptr;
        size_t idx_ = 0;

        friend class result;
    };

} // namespace httplib::db

#include "row.inl"
