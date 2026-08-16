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

    /// SQLite 后端：同步阻塞（接受阻塞当前 io 线程）。
    class sqlite_backend : public backend
    {
      public:
        explicit sqlite_backend(sqlite_config cfg);

        net::awaitable<void> connect() override;
        net::awaitable<void> reconnect() override;
        net::awaitable<bool> ping() override;
        bool alive() const override { return db_ != nullptr; }

        net::awaitable<result> execute(std::string_view sql) override;
        net::awaitable<result> execute(std::string_view sql,
                                       std::vector<param> const& params,
                                       bool cacheable = true) override;

        net::awaitable<void> begin() override;
        net::awaitable<void> commit() override;
        net::awaitable<void> rollback() override;

      private:
        result exec(std::string_view sql, std::vector<param> const& params);

        sqlite_config cfg_;
        sqlite3* db_ = nullptr;
    };

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE
