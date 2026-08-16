#ifdef HTTPLIB_ENABLED_DATABASE
#include "sqlite_backend.hpp"
#include "httplib/db/exception.hpp"
#include "registry.hpp"
#include <sqlite3.h>
#include <stdexcept>

namespace httplib::db::detail
{
    namespace
    {
        // 把公开参数绑定到 sqlite3_stmt（从 1 开始编号）。
        void
        bind_params(sqlite3_stmt* stmt, std::vector<param> const& params)
        {
            int idx = 1;
            for (auto const& p : params)
            {
                std::visit(
                    [&](auto const& v)
                    {
                        using T = std::decay_t<decltype(v)>;
                        int rc = SQLITE_OK;
                        if constexpr (std::is_same_v<T, std::monostate>)
                        {
                            rc = sqlite3_bind_null(stmt, idx);
                        }
                        else if constexpr (std::is_same_v<T, int64_t>)
                        {
                            rc = sqlite3_bind_int64(stmt, idx, v);
                        }
                        else if constexpr (std::is_same_v<T, uint64_t>)
                        {
                            rc = sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(v));
                        }
                        else if constexpr (std::is_same_v<T, double>)
                        {
                            rc = sqlite3_bind_double(stmt, idx, v);
                        }
                        else if constexpr (std::is_same_v<T, std::string>)
                        {
                            rc = sqlite3_bind_text(stmt, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
                        }
                        else if constexpr (std::is_same_v<T, std::span<std::byte const>>)
                        {
                            rc = sqlite3_bind_blob(stmt, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
                        }
                        else if constexpr (std::is_same_v<T, date>)
                        {
                            auto s = v.to_string();
                            rc = sqlite3_bind_text(stmt, idx, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
                        }
                        else if constexpr (std::is_same_v<T, datetime>)
                        {
                            auto s = v.to_string();
                            rc = sqlite3_bind_text(stmt, idx, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
                        }
                        else if constexpr (std::is_same_v<T, time>)
                        {
                            auto s = v.to_string();
                            rc = sqlite3_bind_text(stmt, idx, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
                        }
                        else if constexpr (std::is_same_v<T, param_array>)
                        {
                            // 数组参数已在渲染层展开，不应到达后端。
                            throw db_exception(boost::system::error_code {}, "db: array parameter not expanded");
                        }
                        else
                        {
                            // time_point：SQLite 无时区，按 UTC 存文本
                            auto s = datetime::from_time_point(v).to_string();
                            rc = sqlite3_bind_text(stmt, idx, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
                        }
                        if (rc != SQLITE_OK)
                        {
                            throw db_exception(boost::system::error_code {},
                                               std::string("db: sqlite bind failed: ") + sqlite3_errmsg(nullptr));
                        }
                    },
                    p);
                ++idx;
            }
        }

        // 根据列声明类型（DATE/DATETIME/TIME）判断列语义；SQLite 只有 5 种存储类型。
        db::column_type
        decltype_to_column_type(char const* decl)
        {
            if (!decl)
            {
                return db::column_type::unknown;
            }
            std::string_view d(decl);
            if (d == "DATE")
            {
                return db::column_type::date;
            }
            if (d == "DATETIME" || d == "TIMESTAMP")
            {
                return db::column_type::datetime;
            }
            if (d == "TIME")
            {
                return db::column_type::time;
            }
            return db::column_type::unknown;
        }

        // 读一列，按声明类型把 TEXT 解析成 date/datetime/time。
        field
        column_to_field(sqlite3_stmt* stmt, int col, db::column_type declared)
        {
            int t = sqlite3_column_type(stmt, col);
            switch (t)
            {
                case SQLITE_NULL:
                    return std::monostate {};
                case SQLITE_INTEGER:
                    return static_cast<int64_t>(sqlite3_column_int64(stmt, col));
                case SQLITE_FLOAT:
                    return sqlite3_column_double(stmt, col);
                case SQLITE_BLOB:
                {
                    auto const* p = static_cast<std::byte const*>(sqlite3_column_blob(stmt, col));
                    int n = sqlite3_column_bytes(stmt, col);
                    return std::vector<std::byte>(p, p + n);
                }
                case SQLITE_TEXT:
                {
                    auto const* p = reinterpret_cast<char const*>(sqlite3_column_text(stmt, col));
                    int n = sqlite3_column_bytes(stmt, col);
                    std::string_view sv(p, static_cast<size_t>(n));
                    switch (declared)
                    {
                        case db::column_type::date:
                            if (auto d = date::from_string(sv))
                            {
                                return *d;
                            }
                            break;
                        case db::column_type::datetime:
                            if (auto d = datetime::from_string(sv))
                            {
                                return *d;
                            }
                            break;
                        case db::column_type::time:
                            if (auto d = time::from_string(sv))
                            {
                                return *d;
                            }
                            break;
                        default:
                            break;
                    }
                    return std::string(sv);
                }
                default:
                    return std::monostate {};
            }
        }
    } // namespace

    sqlite_backend::sqlite_backend(sqlite_config cfg) : cfg_(std::move(cfg)) {}

    net::awaitable<void>
    sqlite_backend::connect()
    {
        int rc = sqlite3_open(cfg_.path.c_str(), &db_);
        if (rc != SQLITE_OK)
        {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
            if (db_)
            {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw db_exception(boost::system::error_code {}, std::string("db: sqlite open failed: ") + msg);
        }
        co_return;
    }

    net::awaitable<void>
    sqlite_backend::reconnect()
    {
        if (db_)
        {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        co_await connect();
    }

    net::awaitable<bool>
    sqlite_backend::ping()
    {
        co_return db_ != nullptr;
    }

    result
    sqlite_backend::exec(std::string_view sql, std::vector<param> const& params)
    {
        if (!db_)
        {
            throw db_exception(boost::system::error_code {}, "db: sqlite not connected");
        }

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, std::string(sql).c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            throw db_exception(boost::system::error_code {},
                               std::string("db: sqlite prepare failed: ") + sqlite3_errmsg(db_));
        }

        bind_params(stmt, params);

        result::resultset s;
        int col_count = sqlite3_column_count(stmt);
        s.names.reserve(col_count);
        s.types.reserve(col_count);
        for (int i = 0; i < col_count; ++i)
        {
            s.names.emplace_back(sqlite3_column_name(stmt, i));
            s.types.push_back(decltype_to_column_type(sqlite3_column_decltype(stmt, i)));
        }

        while (true)
        {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW)
            {
                std::vector<field> values;
                values.reserve(col_count);
                for (int i = 0; i < col_count; ++i)
                {
                    values.push_back(column_to_field(stmt, i, s.types[i]));
                }
                s.rows.push_back(std::move(values));
            }
            else if (rc == SQLITE_DONE)
            {
                break;
            }
            else
            {
                sqlite3_finalize(stmt);
                throw db_exception(boost::system::error_code {},
                                   std::string("db: sqlite step failed: ") + sqlite3_errmsg(db_));
            }
        }

        // 只读语句（SELECT 等）不影响行数；sqlite3_changes 只对 INSERT/UPDATE/DELETE 有意义，
        // 否则会把上一条 DML 的计数泄漏给 SELECT 结果。必须在 finalize 之前取值。
        s.affected = sqlite3_stmt_readonly(stmt) ? 0 : static_cast<uint64_t>(sqlite3_changes(db_));
        s.last_insert_id = static_cast<uint64_t>(sqlite3_last_insert_rowid(db_));

        sqlite3_finalize(stmt);

        std::vector<result::resultset> sets;
        sets.push_back(std::move(s));
        return result(std::move(sets), std::chrono::seconds { 0 });
    }

    net::awaitable<result>
    sqlite_backend::execute(std::string_view sql)
    {
        co_return exec(sql, {});
    }

    net::awaitable<result>
    sqlite_backend::execute(std::string_view sql, std::vector<param> const& params, bool /*cacheable*/)
    {
        co_return exec(sql, params);
    }

    net::awaitable<void>
    sqlite_backend::begin()
    {
        co_await execute("BEGIN");
        co_return;
    }

    net::awaitable<void>
    sqlite_backend::commit()
    {
        co_await execute("COMMIT");
        co_return;
    }

    net::awaitable<void>
    sqlite_backend::rollback()
    {
        co_await execute("ROLLBACK");
        co_return;
    }

    void
    register_sqlite_backend()
    {
        register_backend("sqlite",
                         [](net::any_io_executor ex, options const& opts) -> std::unique_ptr<backend>
                         {
                             (void)ex;
                             sqlite_config cfg;
                             cfg.path = opts.get_or("db", opts.get_or("path", cfg.path));
                             return std::make_unique<sqlite_backend>(std::move(cfg));
                         });
    }

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE
