#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/extractor.hpp"
#include "httplib/mysql/mysql_fwd.hpp"
#include "httplib/mysql/result.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace httplib::mysql
{

    /**
     * \brief 预编译语句。
     * \details
     * 由 \ref session::stmt 创建，使用命名占位符 `:name`（内部重写为 MySQL 原生的 `?`）。
     * \n
     * 用法：`bind(...)` 绑定参数后 `execute()` 执行；可用 `execute(into(...))` 一次性把结果提取到变量。
     * \n
     * `:name` 占位符既可按名字绑定（`bind("name", v)`），也可按出现顺序做位置绑定（`bind(v)`，
     * SOCI 风格）；同一语句内不能混用两种方式。
     * \n
     * 绑定参数在语句生命周期内持久：重复 `execute()` 复用上次参数，重新 `bind` 则替换。
     * \n
     * \warning 语句是 \ref session::stmt 创建的临时视图：请勿保存到 session 之外，session 销毁或归还
     *          连接池后语句即失效，再执行属于未定义行为。
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
        prepared_statement& bind(std::span<std::byte const> v);
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
        prepared_statement& bind(std::string_view name, std::span<std::byte const> v);
        prepared_statement& bind(std::string_view name, boost::json::value const& v);
        prepared_statement& bind(std::string_view name, std::chrono::system_clock::time_point tp);

        /**
         * \brief 执行语句。
         * \returns 结果集。
         * \throws mysql_exception 执行失败。
         */
        net::awaitable<result> execute();

        /**
         * \brief 执行语句并把结果提取到变量。
         * \param ex 一个或多个 \ref into 提取声明。
         * \returns 结果集。
         * \throws mysql_exception 执行失败。
         */
        template <typename... Ex>
        net::awaitable<result>
        execute(Ex&&... ex)
        {
            auto res = co_await execute();
            detail::apply_extractors(res, std::forward<Ex>(ex)...);
            co_return res;
        }

        struct impl;
        explicit prepared_statement(session& sess, std::string sql);

      private:
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
