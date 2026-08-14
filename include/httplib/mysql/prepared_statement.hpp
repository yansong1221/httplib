#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/mysql_fwd.hpp"
#include "httplib/mysql/result.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace httplib::mysql
{

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
    } // namespace detail

    /**
     * \brief 预编译语句。
     * \details
     * 由 \ref session::stmt 创建，使用命名占位符 `:name`（内部重写为 MySQL 原生的 `?`）。
     * \n
     * 用法：`bind(...)` 绑定参数后 `execute()` 执行；可选地用 `into(...)` 把结果提取到变量。
     * \n
     * `:name` 占位符既可按名字绑定（`bind("name", v)`），也可按出现顺序做位置绑定（`bind(v)`，
     * SOCI 风格）；同一语句内不能混用两种方式。
     * \n
     * 绑定参数在语句生命周期内持久：重复 `execute()` 复用上次参数，重新 `bind` 则替换；
     * 提取目标同理，重新 `into` 会替换上一次的。
     */
    class HTTPLIB_API prepared_statement
    {
      public:
        prepared_statement(prepared_statement&&) noexcept;
        prepared_statement& operator=(prepared_statement&&) noexcept;
        ~prepared_statement();

        prepared_statement(prepared_statement const&) = delete;
        prepared_statement& operator=(prepared_statement const&) = delete;

        /** 位置式绑定，按调用顺序对应 `:name` 占位符。 */
        prepared_statement& bind(std::string_view v);
        prepared_statement& bind(std::string const& v);
        prepared_statement& bind(char const* v);
        prepared_statement& bind(int64_t v);
        prepared_statement& bind(uint64_t v);
        prepared_statement& bind(int v);
        prepared_statement& bind(unsigned v);
        prepared_statement& bind(short v);
        prepared_statement& bind(unsigned short v);
        prepared_statement& bind(double v);
        prepared_statement& bind(float v);
        prepared_statement& bind(bool v);
        prepared_statement& bind(std::nullptr_t);
        prepared_statement& bind(date v);
        prepared_statement& bind(datetime v);
        prepared_statement& bind(time v);
        prepared_statement& bind(std::span<const std::byte> v);
        prepared_statement& bind(boost::json::value const& v);
        prepared_statement& bind(std::chrono::system_clock::time_point tp);

        /** 命名式绑定（对应 `:name` 占位符，绑定不存在的名字会抛异常）。 */
        prepared_statement& bind(std::string_view name, std::string_view v);
        prepared_statement& bind(std::string_view name, char const* v);
        prepared_statement& bind(std::string_view name, int64_t v);
        prepared_statement& bind(std::string_view name, uint64_t v);
        prepared_statement& bind(std::string_view name, int v);
        prepared_statement& bind(std::string_view name, unsigned v);
        prepared_statement& bind(std::string_view name, short v);
        prepared_statement& bind(std::string_view name, unsigned short v);
        prepared_statement& bind(std::string_view name, double v);
        prepared_statement& bind(std::string_view name, float v);
        prepared_statement& bind(std::string_view name, bool v);
        prepared_statement& bind(std::string_view name, std::nullptr_t);
        prepared_statement& bind(std::string_view name, date v);
        prepared_statement& bind(std::string_view name, datetime v);
        prepared_statement& bind(std::string_view name, time v);
        prepared_statement& bind(std::string_view name, std::span<const std::byte> v);
        prepared_statement& bind(std::string_view name, boost::json::value const& v);
        prepared_statement& bind(std::string_view name, std::chrono::system_clock::time_point tp);

        /**
         * \brief 执行语句。
         * \returns 结果集。
         * \throws mysql_exception 执行失败。
         */
        net::awaitable<result> execute();

        /**
         * \brief 把第一行指定列提取到 optional（NULL → nullopt）。
         * \param v 输出目标。
         * \param col 列下标。
         */
        template <typename T>
        prepared_statement&
        into(std::optional<T>& v, size_t col)
        {
            static_assert(detail::is_optional_into_type_v<T>, "db: unsupported type for into(std::optional)");
            add_extractor([&v, col](result const& r) { v = r[0].get<T>(col); });
            return *this;
        }

        /**
         * \brief 把第一行指定列提取到 optional（NULL → nullopt）。
         * \param v 输出目标。
         * \param name 列名。
         */
        template <typename T>
        prepared_statement&
        into(std::optional<T>& v, std::string_view name)
        {
            static_assert(detail::is_optional_into_type_v<T>, "db: unsupported type for into(std::optional)");
            add_extractor([&v, n = std::string(name)](result const& r) { v = r[0].get<T>(n); });
            return *this;
        }

        /**
         * \brief 把第一行按位置提取到 optional（第 N 次调用对应第 N 列，NULL → nullopt）。
         * \param v 输出目标。
         */
        template <typename T>
        prepared_statement&
        into(std::optional<T>& v)
        {
            static_assert(detail::is_optional_into_type_v<T>, "db: unsupported type for into(std::optional)");
            auto col = alloc_into_col();
            add_extractor([&v, col](result const& r) { v = r[0].get<T>(col); });
            return *this;
        }

        /**
         * \brief 把所有行指定列提取到 vector。
         * \param v 输出目标（先 clear）。
         * \param col 列下标。
         * \throws std::runtime_error 结果中含 NULL。
         */
        template <typename T>
        prepared_statement&
        into(std::vector<T>& v, size_t col)
        {
            static_assert(detail::is_vector_into_type_v<T>, "db: unsupported type for into(std::vector)");
            add_extractor(
                [&v, col](result const& r)
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
                });
            return *this;
        }

        /**
         * \brief 把所有行指定列提取到 vector。
         * \param v 输出目标（先 clear）。
         * \param name 列名。
         * \throws std::runtime_error 结果中含 NULL。
         */
        template <typename T>
        prepared_statement&
        into(std::vector<T>& v, std::string_view name)
        {
            static_assert(detail::is_vector_into_type_v<T>, "db: unsupported type for into(std::vector)");
            add_extractor(
                [&v, n = std::string(name)](result const& r)
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
                });
            return *this;
        }

        /**
         * \brief 把所有行按位置提取到 vector（第 N 次调用对应第 N 列）。
         * \param v 输出目标（先 clear）。
         * \throws std::runtime_error 结果中含 NULL。
         */
        template <typename T>
        prepared_statement&
        into(std::vector<T>& v)
        {
            static_assert(detail::is_vector_into_type_v<T>, "db: unsupported type for into(std::vector)");
            auto col = alloc_into_col();
            add_extractor(
                [&v, col](result const& r)
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
                });
            return *this;
        }

        struct impl;
        explicit prepared_statement(session& sess, std::string sql);

      private:
        void add_extractor(std::function<void(result const&)> ex);

        size_t alloc_into_col();

        std::unique_ptr<impl> impl_;
    };
} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
