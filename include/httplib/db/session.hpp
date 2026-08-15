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

        /// 建立 MySQL 连接。
        static net::awaitable<session> connect(net::any_io_executor ex, mysql_config cfg);

        /// 打开 SQLite 数据库。
        static net::awaitable<session> connect(net::any_io_executor ex, sqlite_config cfg);

        /// 执行一条 SQL 语句。
        net::awaitable<result> query(std::string_view sql);

        /// 执行一条 SQL 语句，可带参数绑定与结果提取。
        template <typename... Ex>
        net::awaitable<result>
        query(std::string_view sql, Ex&&... ex)
        {
            std::vector<detail::binder> binders;
            (detail::collect_binder(binders, std::forward<Ex>(ex)), ...);
            auto res = co_await execute_query(sql, std::move(binders));
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
            std::exception_ptr e;
            try
            {
                co_await std::invoke(std::forward<F>(f), *this);
            }
            catch (...)
            {
                e = std::current_exception();
            }
            if (e)
            {
                try
                {
                    co_await rollback();
                }
                catch (...)
                {
                }
                std::rethrow_exception(e);
            }
            co_await commit();
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

        /// 内部：执行已重写为 `?` 的参数化 SQL（供 \ref prepared_statement 使用）。
        net::awaitable<result> execute_prepared(std::string_view sql,
                                                std::vector<detail::param> params,
                                                std::string_view original_sql,
                                                std::vector<std::string> names);

      private:
        struct impl;
        explicit session(std::unique_ptr<impl> p);

        net::awaitable<result> execute_query(std::string_view sql, std::vector<detail::binder> binders);

        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::db
