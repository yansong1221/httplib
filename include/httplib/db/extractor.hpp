#pragma once

#include "exception.hpp"
#include "httplib/config.hpp"
#include "result.hpp"
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

namespace httplib::db
{

    /**
     * \brief 提取声明（into）：一次性、无状态的输出绑定。
     * \details
     * `into(...)` 返回一个提取器，作为参数传给 \ref session::query 或 \ref prepared_statement::execute，
     * 执行成功后把结果写到用户变量；执行完即弃、不持久。
     * \n
     * 支持三种列寻址：下标 `into(v, 0)`、列名 `into(v, "name")`、位置 `into(v)`（按声明顺序对应第 N 列，
     * SOCI 风格）。
     * \n
     * 目标变量支持三种形态：`std::optional<T>`（NULL → nullopt）、`std::vector<T>`（全部行；含 NULL 抛异常）、
     * 裸标量 `T`（仅第一行；NULL 抛异常）。
     * \n
     * 多个提取器同时使用时，先统一解析各目标列，再一次性遍历结果集，避免每个提取器各自遍历一遍。
     * \warning 提取器持有目标变量的引用，须保证目标变量在 `query`/`execute` 返回前有效。
     */

    namespace detail
    {
        template <typename T>
        inline constexpr bool is_vector_into_type_v
            = std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t> || std::is_same_v<T, int>
              || std::is_same_v<T, unsigned> || std::is_same_v<T, short> || std::is_same_v<T, unsigned short>
              || std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, bool>
              || std::is_same_v<T, std::string> || std::is_same_v<T, text> || std::is_same_v<T, blob>
              || std::is_same_v<T, date> || std::is_same_v<T, datetime> || std::is_same_v<T, time>
              || std::is_same_v<T, timestamp> || std::is_same_v<T, boost::json::value>;

        template <typename T>
        inline constexpr bool is_optional_into_type_v
            = is_vector_into_type_v<T> || std::is_same_v<T, std::span<std::byte const>>
              || std::is_same_v<T, std::string_view>;

        template <typename T>
        inline constexpr bool is_scalar_into_type_v = is_optional_into_type_v<T>;

        /**
         * \brief 提取器：先解析目标列，再按行填充。
         * \details 多提取器场景下由 apply_extractors 先统一 resolve 所有列，再一次性遍历结果集，
         * 每个提取器只处理当前行的目标列。
         */
        struct extractor
        {
            std::function<size_t(size_t& next_col)> resolve_col; ///< 解析目标列（位置序按声明顺序递增）。
            size_t col = 0;                                      ///< 解析出的列号，供 apply 使用。
            std::function<void(row const&, size_t col, size_t row_count)> apply;
            std::function<void()> on_empty;                      ///< 结果集为空时回调（如 vector 提取清空已有数据）。
        };

        template <typename E>
        void
        resolve_one(size_t& next_col, E&& e)
        {
            if constexpr (std::is_same_v<std::decay_t<E>, extractor>)
            {
                e.col = e.resolve_col(next_col);
            }
        }

        template <typename E>
        void
        apply_one(row const& r, size_t row_count, E&& e)
        {
            if constexpr (std::is_same_v<std::decay_t<E>, extractor>)
            {
                e.apply(r, e.col, row_count);
            }
        }

        template <typename E>
        void
        on_empty_one(E&& e)
        {
            if constexpr (std::is_same_v<std::decay_t<E>, extractor>)
            {
                if (e.on_empty)
                {
                    e.on_empty();
                }
            }
        }

        template <typename... Ex>
        void
        apply_extractors(result const& res, Ex&&... ex)
        {
            size_t const n = res.row_count();
            size_t next_col = 0;
            (static_cast<void>(resolve_one(next_col, std::forward<Ex>(ex))), ...);
            if (n == 0)
            {
                // 空结果：仍要让 vector 提取器清空已有数据，避免残留上一批结果。
                (static_cast<void>(on_empty_one(std::forward<Ex>(ex))), ...);
                return;
            }
            for (size_t i = 0; i < n; ++i)
            {
                row r = res[i];
                (static_cast<void>(apply_one(r, n, std::forward<Ex>(ex))), ...);
            }
        }
    } // namespace detail

    /// 把第一行指定列提取到 optional（NULL → nullopt）。
    template <typename T>
    detail::extractor
    into(std::optional<T>& v, size_t col)
    {
        static_assert(detail::is_optional_into_type_v<T>, "db: unsupported type for into(std::optional)");
        return { [col](size_t&) { return col; },
                 col,
                 [&v, first = true](row const& r, size_t c, size_t) mutable
                 {
                     if (first)
                     {
                         v = r.get<T>(c);
                         first = false;
                     }
                 } };
    }

    template <typename T>
    detail::extractor
    into(std::optional<T>& v, std::string_view name)
    {
        static_assert(detail::is_optional_into_type_v<T>, "db: unsupported type for into(std::optional)");
        return { [](size_t&) { return 0; },
                 0,
                 [&v, n = std::string(name), first = true](row const& r, size_t, size_t) mutable
                 {
                     if (first)
                     {
                         v = r.get<T>(n);
                         first = false;
                     }
                 } };
    }

    template <typename T>
    detail::extractor
    into(std::optional<T>& v)
    {
        static_assert(detail::is_optional_into_type_v<T>, "db: unsupported type for into(std::optional)");
        return { [](size_t& next_col) { return next_col++; },
                 0,
                 [&v, first = true](row const& r, size_t c, size_t) mutable
                 {
                     if (first)
                     {
                         v = r.get<T>(c);
                         first = false;
                     }
                 } };
    }

    /// 把所有行指定列提取到 vector（先 clear；含 NULL 抛异常）。
    template <typename T>
    detail::extractor
    into(std::vector<T>& v, size_t col)
    {
        static_assert(detail::is_vector_into_type_v<T>, "db: unsupported type for into(std::vector)");
        return { [col](size_t&) { return col; },
                 col,
                 [&v, first = true](row const& r, size_t c, size_t row_count) mutable
                 {
                     if (first)
                     {
                         v.clear();
                         v.reserve(row_count);
                         first = false;
                     }
                     auto val = r.get<T>(c);
                     if (!val)
                     {
                          throw db_exception(boost::system::error_code {}, "db: NULL value when extracting into vector");
                     }
                     v.push_back(std::move(*val));
                 },
                 [&v] { v.clear(); } };
    }

    template <typename T>
    detail::extractor
    into(std::vector<T>& v, std::string_view name)
    {
        static_assert(detail::is_vector_into_type_v<T>, "db: unsupported type for into(std::vector)");
        return { [](size_t&) { return 0; },
                 0,
                 [&v, n = std::string(name), first = true](row const& r, size_t, size_t row_count) mutable
                 {
                     if (first)
                     {
                         v.clear();
                         v.reserve(row_count);
                         first = false;
                     }
                     auto val = r.get<T>(n);
                     if (!val)
                     {
                          throw db_exception(boost::system::error_code {}, "db: NULL value when extracting into vector");
                     }
                     v.push_back(std::move(*val));
                 },
                 [&v] { v.clear(); } };
    }

    template <typename T>
    detail::extractor
    into(std::vector<T>& v)
    {
        static_assert(detail::is_vector_into_type_v<T>, "db: unsupported type for into(std::vector)");
        return { [](size_t& next_col) { return next_col++; },
                 0,
                 [&v, first = true](row const& r, size_t c, size_t row_count) mutable
                 {
                     if (first)
                     {
                         v.clear();
                         v.reserve(row_count);
                         first = false;
                     }
                     auto val = r.get<T>(c);
                     if (!val)
                     {
                          throw db_exception(boost::system::error_code {}, "db: NULL value when extracting into vector");
                     }
                     v.push_back(std::move(*val));
                 },
                 [&v] { v.clear(); } };
    }

    /// 把第一行指定列提取到裸标量（NULL 抛异常）。
    template <typename T>
    detail::extractor
    into(T& v, size_t col)
    {
        static_assert(detail::is_scalar_into_type_v<T>, "db: unsupported type for into(scalar)");
        return { [col](size_t&) { return col; },
                 col,
                 [&v, first = true](row const& r, size_t c, size_t) mutable
                 {
                     if (first)
                     {
                         auto val = r.get<T>(c);
                         if (!val)
                         {
                              throw db_exception(boost::system::error_code {}, "db: NULL value when extracting into scalar");
                         }
                         v = std::move(*val);
                         first = false;
                     }
                 } };
    }

    template <typename T>
    detail::extractor
    into(T& v, std::string_view name)
    {
        static_assert(detail::is_scalar_into_type_v<T>, "db: unsupported type for into(scalar)");
        return { [](size_t&) { return 0; },
                 0,
                 [&v, n = std::string(name), first = true](row const& r, size_t, size_t) mutable
                 {
                     if (first)
                     {
                         auto val = r.get<T>(n);
                         if (!val)
                         {
                              throw db_exception(boost::system::error_code {}, "db: NULL value when extracting into scalar");
                         }
                         v = std::move(*val);
                         first = false;
                     }
                 } };
    }

    template <typename T>
    detail::extractor
    into(T& v)
    {
        static_assert(detail::is_scalar_into_type_v<T>, "db: unsupported type for into(scalar)");
        return { [](size_t& next_col) { return next_col++; },
                 0,
                 [&v, first = true](row const& r, size_t c, size_t) mutable
                 {
                     if (first)
                     {
                         auto val = r.get<T>(c);
                         if (!val)
                         {
                              throw db_exception(boost::system::error_code {}, "db: NULL value when extracting into scalar");
                         }
                         v = std::move(*val);
                         first = false;
                     }
                 } };
    }

} // namespace httplib::db
