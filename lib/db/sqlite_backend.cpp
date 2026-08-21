#ifdef HTTPLIB_ENABLED_DATABASE
#include "sqlite_backend.hpp"
#include "httplib/db/exception.hpp"
#include "registry.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <cctype>
#include <limits>
#include <sqlite3.h>
#include <stdexcept>

namespace httplib::db::detail
{
    namespace
    {
        // RAII 语句 owner：析构自动 finalize（借鉴 SQLiteCpp Statement::prepareStatement）。
        std::shared_ptr<sqlite3_stmt>
        own_stmt(sqlite3_stmt* stmt)
        {
            return std::shared_ptr<sqlite3_stmt>(stmt, [](sqlite3_stmt* p) { sqlite3_finalize(p); });
        }

        // 展开绑定参数后的 SQL 文本（错误消息用，SQLite >= 3.14）；
        // 存在 NULL 参数或未绑定参数时 sqlite3_expanded_sql 返回 NULL，此时返回空串。
        std::string
        expanded_sql(sqlite3_stmt* stmt)
        {
            char* e = sqlite3_expanded_sql(stmt);
            if (!e)
            {
                return {};
            }
            std::string s(e);
            sqlite3_free(e);
            return s;
        }

        // 统一错误消息：连接 errmsg + sqlite 错误码（借鉴 SQLiteCpp 的 errcode/errstr）
        // +（语句可用时）展开绑定参数后的 SQL。
        std::string
        sqlite_error_msg(sqlite3* db, int rc, std::string_view what, sqlite3_stmt* stmt = nullptr)
        {
            std::string msg
                = std::string("db: ") + std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "out of memory");
            msg += " [sqlite rc=" + std::to_string(rc) + ", " + sqlite3_errstr(rc) + "]";
            if (stmt)
            {
                std::string e = expanded_sql(stmt);
                if (!e.empty())
                {
                    msg += " [expanded: " + e + "]";
                }
            }
            return msg;
        }

        // 把公开参数绑定到 sqlite3_stmt（从 1 开始编号）。
        void
        bind_params(sqlite3* db, sqlite3_stmt* stmt, std::vector<param> const& params)
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
                            // SQLite 只有有符号 64 位整数，超出 INT64_MAX 的 uint64 无法无损存储，
                            // 静默回绕会损坏数据，这里显式报错。
                            if (v > static_cast<uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
                            {
                                throw db_exception(boost::system::error_code {},
                                                   sqlite_error_msg(db,
                                                                    SQLITE_TOOBIG,
                                                                    "sqlite bind failed: uint64 out of range",
                                                                    stmt));
                            }
                            rc = sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(v));
                        }
                        else if constexpr (std::is_same_v<T, double>)
                        {
                            rc = sqlite3_bind_double(stmt, idx, v);
                        }
                        else if constexpr (std::is_same_v<T, text>)
                        {
                            rc = sqlite3_bind_text(stmt,
                                                   idx,
                                                   v.data().data(),
                                                   static_cast<int>(v.data().size()),
                                                   SQLITE_TRANSIENT);
                        }
                        else if constexpr (std::is_same_v<T, blob>)
                        {
                            rc = sqlite3_bind_blob(stmt,
                                                   idx,
                                                   v.data().data(),
                                                   static_cast<int>(v.data().size()),
                                                   SQLITE_TRANSIENT);
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
                        else if constexpr (std::is_same_v<T, timestamp>)
                        {
                            // time_point：SQLite 无时区，按 UTC 存文本
                            auto s = datetime::from_time_point(v).to_string();
                            rc = sqlite3_bind_text(stmt, idx, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
                        }
                        else
                        {
                            static_assert(std::is_same_v<T, timestamp>, "db: unhandled field type in bind_params");
                        }
                        if (rc != SQLITE_OK)
                        {
                            throw db_exception(boost::system::error_code {},
                                               sqlite_error_msg(db, rc, "sqlite bind failed", stmt));
                        }
                    },
                    p);
                ++idx;
            }
        }

        // 根据列声明类型判断列语义；SQLite 只有 5 种存储类型，date/datetime/time 均以 TEXT 存储。
        // 取声明首词并用 boost::iequals 大小写不敏感匹配（schema 里小写 `date` 或带精度修饰
        // `datetime(3)` 都应识别）。
        db::column_type
        decltype_to_column_type(char const* decl)
        {
            if (!decl)
            {
                return db::column_type::unknown;
            }
            std::string_view word(decl);
            size_t len = 0;
            while (len < word.size() && std::isalpha(static_cast<unsigned char>(word[len])))
            {
                ++len;
            }
            word.remove_suffix(word.size() - len);
            if (boost::iequals(word, "date"))
            {
                return db::column_type::date;
            }
            if (boost::iequals(word, "datetime") || boost::iequals(word, "timestamp"))
            {
                return db::column_type::datetime;
            }
            if (boost::iequals(word, "time"))
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
                    return blob(std::vector<std::byte>(p, p + n));
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
                    return text(std::string(sv));
                }
                default:
                    return std::monostate {};
            }
        }

        // 提取语句首词（跳过前导空白），用于按语句类型区分计数语义。
        std::string_view
        stmt_first_word(sqlite3_stmt* stmt)
        {
            auto const* sql = sqlite3_sql(stmt);
            if (!sql)
            {
                return {};
            }
            std::string_view sv(sql);
            size_t i = 0;
            while (i < sv.size() && std::isspace(static_cast<unsigned char>(sv[i])))
            {
                ++i;
            }
            size_t end = i;
            while (end < sv.size() && std::isalpha(static_cast<unsigned char>(sv[end])))
            {
                ++end;
            }
            return sv.substr(i, end - i);
        }

        // 语句是否为 INSERT（含 REPLACE）：只有 INSERT 才更新 last_insert_rowid，
        // UPDATE/DELETE 等写语句读取会拿到上一次 INSERT 的陈旧 rowid。
        bool
        stmt_is_insert(sqlite3_stmt* stmt)
        {
            auto word = stmt_first_word(stmt);
            return boost::iequals(word, "insert") || boost::iequals(word, "replace");
        }

        // 语句是否为 DML（INSERT/REPLACE/UPDATE/DELETE）：只有 DML 才产生 affected 计数；
        // DDL（CREATE/ALTER/DROP）、SELECT、PRAGMA 等不应报告 affected（sqlite3_changes 是
        // 连接级状态、不被 DDL 更新，直接读会泄漏上一次 DML 的陈旧计数）。
        bool
        stmt_is_dml(sqlite3_stmt* stmt)
        {
            auto word = stmt_first_word(stmt);
            return boost::iequals(word, "insert") || boost::iequals(word, "replace")
                || boost::iequals(word, "update") || boost::iequals(word, "delete");
        }

        // 数据库级故障（文件损坏/不是数据库/磁盘错误/被删）：连接本身已不可信，应标记失效；
        // SQL 语法错误（SQLITE_ERROR）等不影响连接可用性。
        bool
        is_db_fault(int rc)
        {
            switch (rc)
            {
                case SQLITE_CORRUPT:
                case SQLITE_NOTADB:
                case SQLITE_FULL:
                case SQLITE_CANTOPEN:
                case SQLITE_IOERR:
                case SQLITE_NOLFS:
                case SQLITE_READONLY:
                    return true;
                default:
                    return false;
            }
        }

        // 执行并收集一条已 prepare/绑定语句的全部行（finalize 前调用）。
        // 只读语句（SELECT 等）的 affected / last_insert_id 置 0：
        // sqlite3_changes / sqlite3_last_insert_rowid 是连接级状态，DML 的计数 / rowid 会泄漏给
        // 后续的 SELECT 结果（MySQL 多结果集中 SELECT 同样不携带插入信息）。
        int
        collect_statement(sqlite3* db, sqlite3_stmt* stmt, result::resultset& s)
        {
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
                int rc = sqlite3_step(stmt);
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
                    return rc; // 调用方负责 finalize 与失效标记
                }
            }
            bool dml = stmt_is_dml(stmt);
            bool insert = stmt_is_insert(stmt);
            s.affected = dml ? static_cast<uint64_t>(sqlite3_changes(db)) : 0;
            s.last_insert_id = insert ? static_cast<uint64_t>(sqlite3_last_insert_rowid(db)) : 0;
            return 0;
        }
    } // namespace

    void
    sqlite3_deleter::operator()(sqlite3* p) const noexcept
    {
        // close_v2 僵尸化连接：即使仍有未 finalize 语句也不会失败，
        // 连接在最后一条语句 finalize 后自动释放（sqlite3_close 此时会返回 BUSY 且连接泄漏）。
        sqlite3_close_v2(p);
    }

    sqlite_backend::sqlite_backend(sqlite_config cfg) : cfg_(std::move(cfg)) {}

    // 析构走 unique_ptr 的 deleter（close_v2 僵尸化）：会话层缓存的语句句柄（RAII owner）
    // 即使未及时 finalize 也可安全晚于本析构 finalize，最后一条语句 finalize 时才真正释放连接。
    sqlite_backend::~sqlite_backend() = default;

    net::awaitable<void>
    sqlite_backend::connect()
    {
        sqlite3* raw = nullptr;
        int rc = sqlite3_open(cfg_.path.c_str(), &raw);
        db_.reset(raw);
        if (rc != SQLITE_OK)
        {
            auto msg = sqlite_error_msg(raw, rc, "sqlite open failed");
            db_.reset();
            throw db_exception(boost::system::error_code {}, std::move(msg));
        }
        if (cfg_.busy_timeout.count() > 0)
        {
            // 忙等待超时：写锁冲突时等待重试而非立即 SQLITE_BUSY（借鉴 SQLiteCpp setBusyTimeout）。
            int rc2 = sqlite3_busy_timeout(db_.get(), static_cast<int>(cfg_.busy_timeout.count()));
            if (rc2 != SQLITE_OK)
            {
                auto msg = sqlite_error_msg(db_.get(), rc2, "sqlite set busy timeout failed");
                db_.reset();
                throw db_exception(boost::system::error_code {}, std::move(msg));
            }
        }
        co_return;
    }

    net::awaitable<void>
    sqlite_backend::reconnect()
    {
        // 重连后会话层已先释放所有缓存语句句柄，这里只关闭并重新打开连接。
        db_.reset();
        co_await connect();
    }

    net::awaitable<bool>
    sqlite_backend::ping()
    {
        if (!db_)
        {
            co_return false;
        }
        try
        {
            // 执行真实 SQL 探测：文件被删/损坏/只读等故障时 report false，
            // 供连接池 validate_on_borrow / 健康检查剔除（仅判 db_ 指针会漏报）。
            co_return exec_all("SELECT 1").row_count() == 1;
        }
        catch (...)
        {
            co_return false;
        }
    }

    void
    sqlite_backend::fail(std::string_view what, int rc, sqlite3_stmt* stmt)
    {
        auto msg = sqlite_error_msg(db_.get(), rc, what, stmt);
        if (is_db_fault(rc))
        {
            // 数据库级故障：reset 触发 close_v2 僵尸化，让仍被缓存持有的语句 finalize 后自动释放；
            // 直接丢指针会泄漏连接（fd/内存/WAL 锁）。
            db_.reset();
        }
        throw db_exception(boost::system::error_code {}, std::move(msg));
    }

    result
    sqlite_backend::exec(std::string_view sql)
    {
        if (!db_)
        {
            throw db_exception(boost::system::error_code {}, "db: sqlite not connected");
        }
        return exec_all(std::string(sql));
    }

    result
    sqlite_backend::exec_all(std::string const& sql)
    {
        std::vector<result::resultset> sets;
        std::vector<std::shared_ptr<sqlite3_stmt>> live;
        size_t tail = 0;
        while (tail < sql.size())
        {
            auto const* start = sql.data() + tail;
            sqlite3_stmt* raw = nullptr;
            auto const* next = start;
            int rc = sqlite3_prepare_v2(db_.get(), start, -1, &raw, &next);
            if (rc != SQLITE_OK)
            {
                fail("sqlite prepare failed", rc);
            }
            if (!raw)
            {
                // 空语句（多余分号等）不产生 stmt：跳到其后继续
                tail = static_cast<size_t>(next - sql.data());
                if (tail <= static_cast<size_t>(start - sql.data()))
                {
                    break; // 防御：无进展时退出
                }
                continue;
            }
            auto owner = own_stmt(raw); // RAII：异常路径由 live 析构统一 finalize
            live.push_back(owner);
            result::resultset s;
            int rc2 = collect_statement(db_.get(), raw, s);
            if (rc2 != 0)
            {
                fail("sqlite step failed", rc2, raw);
            }
            sets.push_back(std::move(s));
            tail = static_cast<size_t>(next - sql.data());
        }
        return result(std::move(sets));
    }

    net::awaitable<result>
    sqlite_backend::execute(std::string_view sql)
    {
        co_return exec(sql);
    }

    net::awaitable<statement_handle>
    sqlite_backend::prepare(std::string_view sql)
    {
        if (!db_)
        {
            throw db_exception(boost::system::error_code {}, "db: sqlite not connected");
        }
        std::string sql_str(sql);
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_.get(), sql_str.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            throw db_exception(boost::system::error_code {}, sqlite_error_msg(db_.get(), rc, "sqlite prepare failed"));
        }
        co_return statement_handle { own_stmt(stmt) };
    }

    net::awaitable<result>
    sqlite_backend::execute_statement(statement_handle h, std::vector<param> const& params)
    {
        auto* stmt = static_cast<sqlite3_stmt*>(h.state.get());
        if (!stmt)
        {
            throw db_exception(boost::system::error_code {}, "db: sqlite statement not prepared");
        }

        // 复用语句：先 reset 再清残留绑定（上一次参数多于本次时会残留 NULL 以外的值）。
        int rc = sqlite3_reset(stmt);
        if (rc == SQLITE_OK)
        {
            rc = sqlite3_clear_bindings(stmt);
        }
        if (rc != SQLITE_OK)
        {
            fail("sqlite reset failed", rc, stmt);
        }
        bind_params(db_.get(), stmt, params);

        result::resultset s;
        rc = collect_statement(db_.get(), stmt, s);
        if (rc != 0)
        {
            // 不在此 finalize：句柄归统一层缓存 / close_statement 管理，避免双重释放
            fail("sqlite step failed", rc, stmt);
        }

        std::vector<result::resultset> sets;
        sets.push_back(std::move(s));
        co_return result(std::move(sets));
    }

    net::awaitable<void>
    sqlite_backend::close_statement(statement_handle h) noexcept
    {
        if (!h.state)
        {
            co_return;
        }
        h.state.reset(); // 触发 finalize（close_v2 后即使连接已僵尸化也安全）
        co_return;
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
                             if (auto bt = opts.as_int("busy_timeout"))
                             {
                                 cfg.busy_timeout = std::chrono::milliseconds(*bt < 0 ? 0 : *bt);
                             }
                             return std::make_unique<sqlite_backend>(std::move(cfg));
                         });
    }

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE
