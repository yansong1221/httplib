#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "backend.hpp"
#include "httplib/db/config.hpp"
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/windows/object_handle.hpp>
#include <functional>
#include <memory>
#include <sql.h>
#include <sqlext.h>
#include <string>
#include <windows.h>

namespace httplib::db::detail
{
    /// ODBC 后端：异步接口（同 MySQL/SQLite 的协程签名）。
    /// \details
    /// 使用 ODBC 3.8 原生异步通知（Asynchronous Execution - Notification Method），不额外开线程：
    /// 连接/语句启用异步后，ODBC 函数可能返回 SQL_STILL_EXECUTING，完成时会信号关联的 Win32 事件；
    /// 本后端把事件交给 asio \c windows::object_handle 挂在 io 线程上等待，事件触发后调用
    /// SQLCompleteAsync 取最终返回码，全程不阻塞事件循环。
    /// \n
    /// 占位符固定为 `?`（SQL 标准），事务用 SQL_ATTR_AUTOCOMMIT 开关 + SQLEndTran 控制。
    class odbc_backend : public backend
    {
      public:
        explicit odbc_backend(net::any_io_executor ex, odbc_config cfg);
        ~odbc_backend() override;

        net::awaitable<void> connect() override;
        net::awaitable<void> reconnect() override;
        net::awaitable<bool> ping() override;
        bool
        alive() const override
        {
            return dbc_ != nullptr && live_;
        }

        net::awaitable<result> execute(std::string_view sql) override;
        net::awaitable<statement_handle> prepare(std::string_view sql) override;
        net::awaitable<result> execute_statement(statement_handle h, std::vector<param> const& params) override;
        net::awaitable<void> close_statement(statement_handle h) noexcept override;

        net::awaitable<void> begin() override;
        net::awaitable<void> commit() override;
        net::awaitable<void> rollback() override;

      private:
        /// 已 prepare 的语句 + 关联的异步通知事件（Win32 auto-reset event）。
        struct odbc_stmt
        {
            SQLHSTMT stmt = nullptr;
            void* event = nullptr; ///< HANDLE
            std::unique_ptr<net::windows::object_handle> obj;
        };

        /// 分配语句句柄并启用语句级异步通知。
        std::unique_ptr<odbc_stmt> new_stmt();
        /// 释放语句资源（object_handle、事件、句柄），不抛异常。
        static void free_stmt(odbc_stmt& s) noexcept;
        /// 异步调用 statement 函数（fn 首次调用；SQL_STILL_EXECUTING 时等事件并 SQLCompleteAsync）。
        net::awaitable<SQLRETURN> stmt_async(odbc_stmt& s, std::function<SQLRETURN()> fn);
        /// 异步调用 connection 函数（同 stmt_async，但用连接级事件）。
        net::awaitable<SQLRETURN> conn_async(std::function<SQLRETURN()> fn);
        /// 读取语句全部结果集（含多结果集）。
        net::awaitable<result> read_all(odbc_stmt& s);
        void disconnect() noexcept;
        /// ODBC 调用失败时检查返回码；连接异常（SQLSTATE 08xxx）时标记 live_ = false 再抛。
        void check_ok(SQLRETURN rc, SQLHANDLE handle, SQLSMALLINT handle_type, std::string_view what);

        net::any_io_executor ex_;
        std::unique_ptr<net::windows::object_handle> conn_obj_;
        odbc_config cfg_;
        void* env_ = nullptr; ///< SQLHENV
        void* dbc_ = nullptr; ///< SQLHDBC
        bool live_ = true;    ///< 连接是否存活（连接异常错误时置 false）
    };

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE