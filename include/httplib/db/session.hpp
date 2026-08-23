#pragma once

#include "binder.hpp"
#include "config.hpp"
#include "extractor.hpp"
#include "httplib/config.hpp"
#include "prepared_statement.hpp"
#include "result.hpp"
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace httplib::db
{

    /// 一条查询的日志记录。
    struct query_log_entry
    {
        std::string sql;
        std::chrono::steady_clock::duration duration { 0 };
        size_t row_count = 0;
        uint64_t affected_rows = 0;
        bool is_parameterized = false;
    };

    /**
     * \brief 一条数据库会话（backend 无关）。
     * \details
     * 会话内的操作都是协程，必须在同一执行上下文中串行使用。
     */
    class HTTPLIB_API session
    {
      public:
        using query_logger = std::function<void(query_log_entry const&)>;

        session(session&&) noexcept;
        session& operator=(session&&) noexcept;
        ~session();

        /// 按后端名 + 连接串建立连接（backend 无关）。
        /// 例：`connect(ex, "mysql", "host=127.0.0.1 user=root password=123456 db=main")`。
        /// 各后端支持的连接串键见 \ref mysql_config 与 \ref sqlite_config 与 \ref odbc_config。
        static net::awaitable<session> connect(net::any_io_executor ex,
                                               std::string_view backend_name,
                                               std::string_view conn_string);

        net::any_io_executor get_executor() const;

        /// 执行一条 SQL 语句。
        net::awaitable<result> query(std::string_view sql);

        /// 执行一条 SQL 语句，可带参数绑定与结果提取。
        template <typename... Ex>
        net::awaitable<result>
        query(std::string_view sql, Ex&&... ex)
        {
            std::vector<detail::binder> binders;
            (detail::collect_binder(binders, std::forward<Ex>(ex)), ...);
            auto res = co_await execute_query(sql, std::move(binders), false);
            detail::apply_extractors(res, std::forward<Ex>(ex)...);
            co_return res;
        }

        /// 创建一个 prepared statement。
        prepared_statement stmt(std::string_view sql);

        net::awaitable<void> begin_transaction();
        net::awaitable<void> commit();
        net::awaitable<void> rollback();

        template <typename F>
            requires std::invocable<F, session&>
                     && std::same_as<std::invoke_result_t<F, session&>, net::awaitable<void>>
        net::awaitable<void>
        with_transaction(F&& f)
        {
            co_await begin_transaction();

            try
            {
                co_await std::invoke(std::forward<F>(f), *this);
                co_await commit();
            }
            catch (...)
            {
                auto e = std::current_exception();

                try
                {
                    co_await rollback();
                }
                catch (...)
                {
                }

                std::rethrow_exception(e);
            }
        }

        /// 探测连接是否存活。
        net::awaitable<bool> ping();

        /// 断开并重新建立连接（保留原连接配置）。
        net::awaitable<void> reconnect();

        void set_query_logger(query_logger cb);
        /// 更新最后活动时间（供连接池调用）。
        void touch();
        /// 连接是否存活（供连接池调用）。
        bool is_live() const;
        std::chrono::steady_clock::time_point last_active_time() const;
        std::chrono::steady_clock::time_point last_ping_time() const;
        bool in_transaction() const;

      private:
        friend class prepared_statement;

        struct impl;
        explicit session(std::shared_ptr<impl> p);

        /// 统一连接入口：注册表查后端工厂并建立连接。
        static net::awaitable<session> connect_internal(net::any_io_executor ex,
                                                        std::string_view backend_name,
                                                        options opts);

        /// 内部：执行已渲染的参数化语句（`?` SQL + 平铺参数），统一错误处理 / touch / 日志。
        /// 参数化语句经统一层 prepared statement 缓存（LRU）执行，cacheable=false 时每次 prepare→close。
        net::awaitable<result> execute_rendered(std::string_view sql,
                                                std::vector<detail::param> params,
                                                std::string_view original_sql,
                                                std::vector<std::string> const& names,
                                                bool cacheable);
        /// 内部：收集/渲染参数化 SQL（binders → `?` + 平铺参数）并执行。
        /// query 与 prepared_statement 共用；数组参数会展开为多个 `?` 并自动跳过缓存。
        net::awaitable<result> execute_query(std::string_view sql, std::vector<detail::binder> binders, bool cacheable);

        /// 析构/移动赋值时把 impl 交给后台协程：异步 close 所有缓存语句后自毁。
        void detach() noexcept;

        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::db
