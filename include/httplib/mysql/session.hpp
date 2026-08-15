#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/config.hpp"
#include "httplib/mysql/prepared_statement.hpp"
#include "httplib/mysql/result.hpp"
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

namespace httplib::mysql
{

    /**
     * \brief 一条查询的日志记录。
     * \details 用于 \ref session::set_query_logger。
     */
    struct query_log_entry
    {
        /// 执行的 SQL（命名参数保留原始 `:name` 形式）。
        std::string sql;
        /// 执行耗时。
        std::chrono::steady_clock::duration duration { 0 };
        /// 返回的行数。
        size_t row_count = 0;
        /// 影响的行数。
        uint64_t affected_rows = 0;
        /// 是否为参数化查询。
        bool is_parameterized = false;
    };

    /**
     * \brief 一条 MySQL 连接的会话。
     * \details
     * 可通过 \ref connect 独立创建，或由 \ref connection_pool 借出。
     * \n
     * 会话内的操作（query / stmt / 事务）都是协程，必须在同一执行上下文中串行使用。
     */
    class HTTPLIB_API session
    {
      public:
        using query_logger = std::function<void(query_log_entry const&)>;

        session(session&&) noexcept;
        session& operator=(session&&) noexcept;
        ~session();

        /**
         * \brief 建立一条新连接。
         * \param ex 执行器。
         * \param cfg 连接参数。
         * \returns 连接好的会话。
         * \throws boost::system::system_error 连接失败或超时。
         */
        static net::awaitable<session> connect(net::any_io_executor ex, connect_params cfg);

        /**
         * \brief 执行一条 SQL 语句。
         * \param sql SQL 文本。
         * \returns 结果集。
         * \throws mysql_exception 执行失败。
         */
        net::awaitable<result> query(std::string_view sql);

        /**
         * \brief 创建一个 prepared statement（支持 `?` 与 `:name` 占位符）。
         * \param sql SQL 文本。
         * \warning 返回的语句是本次会话的临时视图：不要保存到 session 之外，session 销毁或归还连接池后
         *          语句即失效，再调用 execute 属于未定义行为。
         */
        prepared_statement stmt(std::string_view sql);

        net::awaitable<void> begin_transaction();
        net::awaitable<void> commit();
        net::awaitable<void> rollback();

        /**
         * \brief 在事务中执行 `f`，异常时回滚并重新抛出，否则提交。
         * \param f 事务体，签名为 `net::awaitable<void>(session&)`。
         * \throws 事务体中抛出的异常（回滚失败不掩盖原始异常）。
         */
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
                    // 回滚失败（通常是连接已死）时不掩盖原始异常
                }
                std::rethrow_exception(e);
            }
            co_await commit();
        }

        /**
         * \brief 探测连接是否存活。
         * \returns 存活返回 true；失败返回 false 并标记连接失效。
         */
        net::awaitable<bool> ping();

        /**
         * \brief 关闭并重建连接（清除语句缓存）。
         * \throws boost::system::system_error 重连失败。
         */
        net::awaitable<void> reconnect();

        /**
         * \brief 设置查询日志回调。
         */
        void set_query_logger(query_logger cb);

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        std::chrono::steady_clock::time_point last_active_time() const;
        std::chrono::steady_clock::time_point last_ping_time() const;
        /**
         * \brief 当前是否处于未提交的事务中。
         */
        bool in_transaction() const;

        struct impl;
        explicit session(std::unique_ptr<impl> p);

      private:
        std::unique_ptr<impl> impl_;

        friend impl& get_impl(session& self);
        friend impl const& get_impl(session const& self);
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
