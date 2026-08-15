#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/result.hpp"
#include <boost/json/value.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace httplib::mysql
{

    /**
     * \brief 提取声明（into）：一次性、无状态的输出绑定。
     * \details
     * `into(...)` 返回一个提取器，作为参数传给 \ref session::query 或 \ref prepared_statement::execute，
     * 执行成功后把结果写到用户变量；执行完即弃、不持久。
     * \n
     * 支持三种列寻址：下标 `into(v, 0)`、列名 `into(v, "name")`、位置 `into(v)`（按声明顺序对应第 N 列，
     * SOCI 风格）。
     * \warning 提取器持有目标变量的引用，须保证目标变量在 `query`/`execute` 返回前有效。
     */

    namespace detail
    {
        /**
         * \brief `into(std::vector<T>&)` 支持的元素类型。
         * \details 仅包含可拥有/可复制的值类型；view 类型（string_view / std::span<const std::byte>）被排除。
         */
        template <typename T>
        inline constexpr bool is_vector_into_type_v
            = std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t> || std::is_same_v<T, int>
              || std::is_same_v<T, unsigned> || std::is_same_v<T, short> || std::is_same_v<T, unsigned short>
              || std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, bool>
              || std::is_same_v<T, std::string> || std::is_same_v<T, date> || std::is_same_v<T, datetime>
              || std::is_same_v<T, time> || std::is_same_v<T, std::chrono::system_clock::time_point>
              || std::is_same_v<T, boost::json::value>;

        /**
         * \brief `into(std::optional<T>&)` 支持的元素类型（在 vector 基础上额外支持 std::span<const std::byte>）。
         */
        template <typename T>
        inline constexpr bool is_optional_into_type_v
            = is_vector_into_type_v<T> || std::is_same_v<T, std::span<const std::byte>>;

        /// 提取器签名 `(result const&, size_t& next_col)`；位置式 into 使用并递增 next_col。
        using extractor = std::function<void(result const&, size_t& next_col)>;

        template <typename E>
        void
        apply_one(result const& res, size_t& next_col, E&& e)
        {
            if constexpr (std::is_same_v<std::decay_t<E>, extractor>)
            {
                e(res, next_col);
            }
            // 其他类型（如 binder）跳过
        }

        template <typename... Ex>
        void
        apply_extractors(result const& res, Ex&&... ex)
        {
            if (res.row_count() == 0)
            {
                return;
            }
            size_t next_col = 0;
            (apply_one(res, next_col, std::forward<Ex>(ex)), ...);
        }
    } // namespace detail

    /// 把第一行指定列提取到 optional（NULL → nullopt）。
    template <typename T>
    detail::extractor
    into(std::optional<T>& v, size_t col)
    {
        static_assert(detail::is_optional_into_type_v<T>, "db: unsupported type for into(std::optional)");
        return [&v, col](result const& r, size_t&) { v = r[0].get<T>(col); };
    }

    /// 把第一行指定列（列名）提取到 optional（NULL → nullopt）。
    template <typename T>
    detail::extractor
    into(std::optional<T>& v, std::string_view name)
    {
        static_assert(detail::is_optional_into_type_v<T>, "db: unsupported type for into(std::optional)");
        return [&v, n = std::string(name)](result const& r, size_t&) { v = r[0].get<T>(n); };
    }

    /// 把第一行按位置提取到 optional（按声明顺序对应第 N 列，NULL → nullopt）。
    template <typename T>
    detail::extractor
    into(std::optional<T>& v)
    {
        static_assert(detail::is_optional_into_type_v<T>, "db: unsupported type for into(std::optional)");
        return [&v](result const& r, size_t& col) { v = r[0].get<T>(col++); };
    }

    /// 把所有行指定列提取到 vector（先 clear；含 NULL 抛异常）。
    template <typename T>
    detail::extractor
    into(std::vector<T>& v, size_t col)
    {
        static_assert(detail::is_vector_into_type_v<T>, "db: unsupported type for into(std::vector)");
        return [&v, col](result const& r, size_t&)
        {
            v.clear();
            v.reserve(r.row_count());
            for (size_t i = 0; i < r.row_count(); ++i)
            {
                auto val = r[i].get<T>(col);
                if (!val)
                {
                    throw std::runtime_error("db: NULL value when extracting into vector");
                }
                v.push_back(std::move(*val));
            }
        };
    }

    /// 把所有行指定列（列名）提取到 vector（先 clear；含 NULL 抛异常）。
    template <typename T>
    detail::extractor
    into(std::vector<T>& v, std::string_view name)
    {
        static_assert(detail::is_vector_into_type_v<T>, "db: unsupported type for into(std::vector)");
        return [&v, n = std::string(name)](result const& r, size_t&)
        {
            v.clear();
            if (r.row_count() == 0)
            {
                return;
            }
            size_t col = r.column_index(n);
            v.reserve(r.row_count());
            for (size_t i = 0; i < r.row_count(); ++i)
            {
                auto val = r[i].get<T>(col);
                if (!val)
                {
                    throw std::runtime_error("db: NULL value when extracting into vector");
                }
                v.push_back(std::move(*val));
            }
        };
    }

    /// 把所有行按位置提取到 vector（按声明顺序对应第 N 列；先 clear；含 NULL 抛异常）。
    template <typename T>
    detail::extractor
    into(std::vector<T>& v)
    {
        static_assert(detail::is_vector_into_type_v<T>, "db: unsupported type for into(std::vector)");
        return [&v](result const& r, size_t& col)
        {
            size_t c = col++;
            v.clear();
            v.reserve(r.row_count());
            for (size_t i = 0; i < r.row_count(); ++i)
            {
                auto val = r[i].get<T>(c);
                if (!val)
                {
                    throw std::runtime_error("db: NULL value when extracting into vector");
                }
                v.push_back(std::move(*val));
            }
        };
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
