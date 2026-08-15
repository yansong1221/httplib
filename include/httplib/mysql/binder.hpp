#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/temporal.hpp"
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/mysql/field_view.hpp>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace httplib::mysql
{

    /**
     * \brief 参数绑定声明（bind）：持有参数值，可传给 \ref session::query 或 \ref prepared_statement。
     * \details
     * `bind(v)` 为位置绑定，`bind("name", v)` 为命名绑定（对应 SQL 里的 `:name` 占位符）。
     * \n
     * \warning blob（std::span<const std::byte>）以非拥有视图存储，调用方需保证缓冲区在 `query`/`execute`
     *          返回前有效。
     */

    namespace detail
    {
        /// 参数值：field_view 为标量/非拥有视图；std::string 为拥有型字符串/JSON 序列化结果；time_point 延迟时区转换。
        using param = std::variant<boost::mysql::field_view, std::string, std::chrono::system_clock::time_point>;

        /// 参数绑定声明：name 为空表示位置绑定，否则为命名绑定。
        struct binder
        {
            std::string name;
            param value;
        };

        // ---- 值 → param ----
        inline param to_param(std::string_view v) { return std::string(v); }
        inline param to_param(std::string const& v) { return v; }
        inline param to_param(char const* v) { return std::string(v); }
        inline param to_param(int64_t v) { return boost::mysql::field_view(v); }
        inline param to_param(uint64_t v) { return boost::mysql::field_view(v); }
        inline param to_param(int v) { return boost::mysql::field_view(static_cast<int64_t>(v)); }
        inline param to_param(unsigned v) { return boost::mysql::field_view(static_cast<uint64_t>(v)); }
        inline param to_param(short v) { return boost::mysql::field_view(static_cast<int64_t>(v)); }
        inline param to_param(unsigned short v) { return boost::mysql::field_view(static_cast<uint64_t>(v)); }
        inline param to_param(double v) { return boost::mysql::field_view(v); }
        inline param to_param(float v) { return boost::mysql::field_view(static_cast<double>(v)); }
        inline param to_param(bool v) { return boost::mysql::field_view(static_cast<int64_t>(v ? 1 : 0)); }
        inline param to_param(std::nullptr_t) { return boost::mysql::field_view(); }
        inline param to_param(date v)
        {
            return boost::mysql::field_view(boost::mysql::date(v.year, v.month, v.day));
        }
        inline param to_param(datetime v)
        {
            return boost::mysql::field_view(
                boost::mysql::datetime(v.year, v.month, v.day, v.hour, v.minute, v.second, v.microsecond));
        }
        inline param to_param(time v) { return boost::mysql::field_view(v.to_duration()); }
        inline param to_param(std::span<const std::byte> v)
        {
            return boost::mysql::field_view(
                boost::mysql::blob_view(reinterpret_cast<unsigned char const*>(v.data()), v.size()));
        }
        inline param to_param(boost::json::value const& v) { return boost::json::serialize(v); }
        inline param to_param(std::chrono::system_clock::time_point tp) { return tp; }

        /// 渲染带参数 SQL（`:name` → 参数文本）。定义在 lib/mysql/session_impl.cpp。
        std::string render_query(std::string_view sql, std::vector<binder> const& binders,
                                 std::chrono::seconds utc_offset);

        /// 把单个字段值渲染为 SQL 字面量（字符串/数值/日期/时间/二进制）。定义在 lib/mysql/session_impl.cpp。
        std::string format_param(boost::mysql::field_view const& f);

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

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
