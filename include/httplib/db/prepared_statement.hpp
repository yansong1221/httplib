#pragma once

#include "binder.hpp"
#include "extractor.hpp"
#include "fwd.hpp"
#include "httplib/config.hpp"
#include "result.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json/value.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace httplib::db
{

    /**
     * \brief 预编译语句。
     * \details
     * 由 \ref session::stmt 创建，使用命名占位符 `:name`（内部重写为 `?`）。
     * \n
     * `bind(...)` 绑定参数后 `execute()` 执行；可用 `execute(into(...))` 一次性把结果提取到变量。
     * \n
     * `:name` 占位符既可按名字绑定（`bind("name", v)`），也可按出现顺序做位置绑定（`bind(v)`）；
     * 同一语句内不能混用两种方式。
     * \n
     * \warning 语句是 \ref session::stmt 创建的临时视图：请勿保存到 session 之外，session 销毁后
     *          语句即失效。
     */
    class HTTPLIB_API prepared_statement
    {
      public:
        prepared_statement(prepared_statement&&) noexcept;
        prepared_statement& operator=(prepared_statement&&) noexcept;
        ~prepared_statement();

        prepared_statement(prepared_statement const&) = delete;
        prepared_statement& operator=(prepared_statement const&) = delete;

        /** 位置式绑定，按调用顺序对应 `:name` 占位符。
         *  数组（std::vector / std::array）会展开为多个 `?`（如 `IN (:ids)`）。 */
        template <typename T>
        prepared_statement&
        bind(T&& v)
        {
            return bind_param(detail::to_param(std::forward<T>(v)));
        }

        /** 命名式绑定（对应 `:name` 占位符，绑定不存在的名字会抛异常）。 */
        template <typename T>
        prepared_statement&
        bind(std::string_view name, T&& v)
        {
            return bind_param(name, detail::to_param(std::forward<T>(v)));
        }

        /// 执行语句。
        net::awaitable<result> execute();

        /// 执行语句并把结果提取到变量。
        /// 参数里的 `db::bind(...)` 会作为额外绑定合并进语句（命名做名字校验，位置按序追加）。
        template <typename... Ex>
        net::awaitable<result>
        execute(Ex&&... ex)
        {
            std::vector<detail::binder> extra;
            (detail::collect_binder(extra, std::forward<Ex>(ex)), ...);
            for (auto& b : extra)
            {
                if (b.name.empty())
                {
                    bind_param(std::move(b.value));
                }
                else
                {
                    bind_param(b.name, std::move(b.value));
                }
            }
            auto res = co_await execute();
            detail::apply_extractors(res, std::forward<Ex>(ex)...);
            co_return res;
        }

        struct impl;
        explicit prepared_statement(session& sess, std::string sql);

      private:
        prepared_statement& bind_param(detail::param v);
        prepared_statement& bind_param(std::string_view name, detail::param v);

        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::db
