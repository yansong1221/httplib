#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "backend.hpp"
#include "httplib/db/config.hpp"
#include <memory>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace httplib::db::detail
{

    /// sqlite3 连接句柄释放器：close_v2 僵尸化（有未 finalize 语句也不失败，
    /// 连接在最后一条语句 finalize 后自动释放）。operator() 定义在实现文件（依赖 sqlite3.h）。
    struct sqlite3_deleter
    {
        void operator()(sqlite3* p) const noexcept;
    };

    /// SQLite 后端：同步阻塞（接受阻塞当前 io 线程）。
    class sqlite_backend : public backend
    {
      public:
        explicit sqlite_backend(sqlite_config cfg);
        ~sqlite_backend() override;

        net::awaitable<void> connect() override;
        net::awaitable<void> reconnect() override;
        net::awaitable<bool> ping() override;
        bool
        alive() const override
        {
            return db_ != nullptr;
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
        /// 逐条 prepare → 绑定 → 执行，收集全部结果集（多语句 SQL 与 MySQL multi_queries 对齐）。
        result exec_all(std::string const& sql, std::vector<param> const& params);
        /// 抛 db_exception；数据库级故障（文件损坏等）时 reset db_ 标记失效。
        /// \p stmt 非空时错误消息附带展开绑定参数后的 SQL。
        void fail(std::string_view what, int rc, sqlite3_stmt* stmt = nullptr);

        sqlite_config cfg_;
        /// sqlite3 连接：unique_ptr + sqlite3_deleter（close_v2 僵尸化，见 deleter 注释）。
        std::unique_ptr<sqlite3, sqlite3_deleter> db_;
        /// 语句句柄 state 直接持有 `std::shared_ptr<sqlite3_stmt>`（RAII owner，析构自动 finalize）。
        /// 句柄由统一层（session）缓存并管理生命周期，close_statement 为显式释放点；
        /// 配合 close_v2，语句即使晚于后端析构 finalize 也安全。
    };

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE
