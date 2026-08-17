#pragma once

#include "httplib/config.hpp"
#include "temporal.hpp"
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace httplib::db
{

    /**
     * \brief 参数绑定声明（bind）：持有参数值，可传给 \ref session::query 或 \ref prepared_statement。
     * \details
     * `bind(v)` 为位置绑定，`bind("name", v)` 为命名绑定（对应 SQL 里的 `:name` 占位符）。
     * \n
     * `std::optional<T>` 可作语法糖：有值 → 绑定值，nullopt → 绑定 NULL。
     * \n
     * \warning blob（std::span<const std::byte>）以非拥有视图存储，调用方需保证缓冲区在 `query`/`execute`
     *          返回前有效。
     */

    namespace detail
    {
        /// 参数值：monostate 表示 NULL；std::string 为拥有型字符串/JSON 序列化结果；std::span 为 blob 视图；
        /// time_point 延迟到渲染时做时区换算。
        using param = std::variant<std::monostate,
                                   int64_t,
                                   uint64_t,
                                   double,
                                   std::string,
                                   std::span<std::byte const>,
                                   date,
                                   datetime,
                                   time,
                                   std::chrono::system_clock::time_point>;

        /// 参数绑定声明：name 为空表示位置绑定，否则为命名绑定。
        struct binder
        {
            std::string name;
            param value;
        };

        // ---- 值 → param ----
        inline param
        to_param(std::string_view v)
        {
            return std::string(v);
        }
        inline param
        to_param(std::string const& v)
        {
            return v;
        }
        inline param
        to_param(char const* v)
        {
            return std::string(v);
        }
        inline param
        to_param(int64_t v)
        {
            return v;
        }
        inline param
        to_param(uint64_t v)
        {
            return v;
        }
        inline param
        to_param(int v)
        {
            return static_cast<int64_t>(v);
        }
        inline param
        to_param(unsigned v)
        {
            return static_cast<uint64_t>(v);
        }
        inline param
        to_param(short v)
        {
            return static_cast<int64_t>(v);
        }
        inline param
        to_param(unsigned short v)
        {
            return static_cast<uint64_t>(v);
        }
        inline param
        to_param(double v)
        {
            return v;
        }
        inline param
        to_param(float v)
        {
            return static_cast<double>(v);
        }
        inline param
        to_param(bool v)
        {
            return static_cast<int64_t>(v ? 1 : 0);
        }
        inline param
        to_param(std::nullptr_t)
        {
            return std::monostate {};
        }
        inline param
        to_param(date v)
        {
            return v;
        }
        inline param
        to_param(datetime v)
        {
            return v;
        }
        inline param
        to_param(time v)
        {
            return v;
        }
        inline param
        to_param(std::span<std::byte const> v)
        {
            return v;
        }
        inline param
        to_param(boost::json::value const& v)
        {
            return boost::json::serialize(v);
        }
        inline param
        to_param(std::chrono::system_clock::time_point tp)
        {
            return tp;
        }

        /// std::optional：有值 → 值；nullopt → NULL。
        template <typename T>
        param
        to_param(std::optional<T> v)
        {
            if (v)
            {
                return to_param(std::move(*v));
            }
            return std::monostate {};
        }

        /// 从混合变参里收集 binder（非 binder 跳过）。
        template <typename E>
        void
        collect_binder(std::vector<binder>& out, E&& e)
        {
            if constexpr (std::is_same_v<std::decay_t<E>, binder>)
            {
                out.push_back(std::forward<E>(e));
            }
        }
    } // namespace detail

    /// 公共别名：参数类型与转换函数（构造异构数组绑定时可显式使用）。
    using detail::param;
    using detail::to_param;

    /// 位置式参数绑定（按出现顺序对应 `:name` 占位符）。
    template <typename T>
    inline detail::binder
    bind(T&& v)
    {
        return { {}, detail::to_param(std::forward<T>(v)) };
    }

    /// 命名式参数绑定（对应 `:name` 占位符）。
    template <typename T>
    inline detail::binder
    bind(std::string_view name, T&& v)
    {
        return { std::string(name), detail::to_param(std::forward<T>(v)) };
    }

} // namespace httplib::db
