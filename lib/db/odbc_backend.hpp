#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "backend.hpp"
#include "httplib/db/config.hpp"
#include <memory>
#include <string>

namespace httplib::db::detail
{

    /// ODBC 后端：同步阻塞（接受阻塞当前 io 线程），通过 ODBC C API 与底层数据库交互。
    /// 占位符固定为 `?`（SQL 标准），事务用 SQL_ATTR_AUTOCOMMIT 开关 + SQLEndTran 控制。
    class odbc_backend : public backend
    {
      public:
        explicit odbc_backend(odbc_config cfg);
        ~odbc_backend() override;

        net::awaitable<void> connect() override;
        net::awaitable<void> reconnect() override;
        net::awaitable<bool> ping() override;
        bool
        alive() const override
        {
            return dbc_ != nullptr;
        }

        net::awaitable<result> execute(std::string_view sql) override;
        net::awaitable<statement_handle> prepare(std::string_view sql) override;
        net::awaitable<result> execute_statement(statement_handle h, std::vector<param> const& params) override;
        net::awaitable<void> close_statement(statement_handle h) noexcept override;

        net::awaitable<void> begin() override;
        net::awaitable<void> commit() override;
        net::awaitable<void> rollback() override;

      private:
        result exec(std::string_view sql, std::vector<param> const& params);
        void disconnect() noexcept;

        odbc_config cfg_;
        void* env_ = nullptr; ///< SQLHENV
        void* dbc_ = nullptr; ///< SQLHDBC
    };

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE