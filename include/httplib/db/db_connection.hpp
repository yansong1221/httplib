#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/db/db_result.hpp"
#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace httplib::db
{

    class db_connection
    {
      public:
        virtual ~db_connection() = default;

        [[deprecated("Unsafe: use query(sql, params) to prevent SQL injection")]] [[nodiscard]] virtual net::awaitable<
            db_result>
        query(std::string_view sql) = 0;

        [[nodiscard]] virtual net::awaitable<db_result> query(std::string_view sql,
                                                              std::span<const std::string> params) = 0;

        [[deprecated("Unsafe: use execute(sql, params) to prevent SQL injection")]] [[nodiscard]] virtual net::awaitable<
            db_result>
        execute(std::string_view sql) = 0;

        [[nodiscard]] virtual net::awaitable<db_result> execute(std::string_view sql,
                                                                std::span<const std::string> params) = 0;

        virtual net::awaitable<void> begin_transaction() = 0;
        virtual net::awaitable<void> commit() = 0;
        virtual net::awaitable<void> rollback() = 0;
        virtual bool in_transaction() const = 0;

        virtual bool is_alive() const = 0;
        [[nodiscard]] virtual net::awaitable<bool> ping() = 0;

        [[nodiscard]] virtual std::string_view backend() const = 0;
        [[nodiscard]] virtual std::chrono::steady_clock::time_point last_active_time() const = 0;
        [[nodiscard]] virtual std::chrono::steady_clock::time_point last_ping_time() const = 0;
        virtual void touch() = 0;
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
