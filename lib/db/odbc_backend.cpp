#ifdef HTTPLIB_ENABLED_DATABASE
#include "odbc_backend.hpp"
#include "httplib/db/exception.hpp"
#include "registry.hpp"
#include <sql.h>
#include <sqlext.h>
#include <windows.h>
#ifndef SQL_TYPE_UTCDATETIME
#define SQL_TYPE_UTCDATETIME (-155)
#endif
#ifndef SQL_SS_TIMESTAMPOFFSET
#define SQL_SS_TIMESTAMPOFFSET (-140)
#endif
#ifndef SQL_SS_TIME2
#define SQL_SS_TIME2 (-154)
#endif
#ifndef SQL_SS_TIMESTAMP2
#define SQL_SS_TIMESTAMP2 (-153)
#endif
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace httplib::db::detail
{
    namespace
    {
        // 获取 ODBC 错误文本（首个诊断记录），供异常消息使用。
        std::string
        odbc_error_text(SQLHANDLE handle, SQLSMALLINT handle_type)
        {
            SQLCHAR state[6] = {};
            SQLCHAR msg[1024] = {};
            SQLSMALLINT msg_len = 0;
            SQLINTEGER native = 0;
            SQLGetDiagRec(handle_type, handle, 1, state, &native, msg, static_cast<SQLSMALLINT>(sizeof(msg)), &msg_len);
            return std::string("odbc error [") + reinterpret_cast<char*>(state) + "] "
                   + std::string(reinterpret_cast<char*>(msg), static_cast<size_t>(msg_len)) + " (native "
                   + std::to_string(native) + ")";
        }

        void
        check_ok(SQLRETURN rc, SQLHANDLE handle, SQLSMALLINT handle_type, std::string_view what)
        {
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
            {
                throw db_exception(boost::system::error_code {},
                                   "db: " + std::string(what) + ": " + odbc_error_text(handle, handle_type));
            }
        }

        /// 单个已绑定的参数（buffer 生命周期覆盖 SQLExecute 全程）。
        struct odbc_bind
        {
            SQLSMALLINT ctype = SQL_C_DEFAULT;
            SQLSMALLINT sqltype = SQL_TYPE_NULL;
            SQLULEN colsize = 0;
            SQLSMALLINT dec = 0;
            SQLLEN len = 0; ///< BufferLength
            SQLLEN ind = 0; ///< 数据长度 / SQL_NULL_DATA
            std::variant<std::monostate,
                         int64_t,
                         uint64_t,
                         double,
                         std::string,
                         std::vector<std::byte>,
                         SQL_DATE_STRUCT,
                         SQL_TIME_STRUCT,
                         SQL_TIMESTAMP_STRUCT>
                data;

            void*
            ptr()
            {
                return std::visit(
                    [](auto& d) -> void*
                    {
                        using T = std::decay_t<decltype(d)>;
                        if constexpr (std::is_same_v<T, std::monostate>)
                        {
                            return nullptr;
                        }
                        else if constexpr (std::is_same_v<T, std::string>)
                        {
                            return const_cast<char*>(d.c_str());
                        }
                        else if constexpr (std::is_same_v<T, std::vector<std::byte>>)
                        {
                            return d.data();
                        }
                        else
                        {
                            return &d;
                        }
                    },
                    data);
            }
        };

        odbc_bind
        make_bind(param const& p)
        {
            odbc_bind b;
            std::visit(
                [&](auto const& v)
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                    {
                        // NULL 绑定：ValueType/SqlType 不能是 SQL_TYPE_NULL，声明为 VARCHAR 即可。
                        b.ctype = SQL_C_CHAR;
                        b.sqltype = SQL_VARCHAR;
                        b.colsize = 1;
                        b.len = 1;
                        b.ind = SQL_NULL_DATA;
                    }
                    else if constexpr (std::is_same_v<T, int64_t>)
                    {
                        b.ctype = SQL_C_SBIGINT;
                        b.sqltype = SQL_BIGINT;
                        b.colsize = 20;
                        b.len = sizeof(int64_t);
                        b.ind = sizeof(int64_t);
                        b.data = v;
                    }
                    else if constexpr (std::is_same_v<T, uint64_t>)
                    {
                        b.ctype = SQL_C_UBIGINT;
                        b.sqltype = SQL_BIGINT;
                        b.colsize = 20;
                        b.len = sizeof(uint64_t);
                        b.ind = sizeof(uint64_t);
                        b.data = v;
                    }
                    else if constexpr (std::is_same_v<T, double>)
                    {
                        b.ctype = SQL_C_DOUBLE;
                        b.sqltype = SQL_DOUBLE;
                        b.colsize = 15;
                        b.len = sizeof(double);
                        b.ind = sizeof(double);
                        b.data = v;
                    }
                    else if constexpr (std::is_same_v<T, std::string>)
                    {
                        b.ctype = SQL_C_CHAR;
                        b.sqltype = SQL_VARCHAR;
                        b.colsize = static_cast<SQLULEN>(v.size());
                        b.len = static_cast<SQLLEN>(v.size()) + 1;
                        b.ind = static_cast<SQLLEN>(v.size());
                        b.data = v;
                    }
                    else if constexpr (std::is_same_v<T, std::span<std::byte const>>)
                    {
                        std::vector<std::byte> blob(v.begin(), v.end());
                        b.ctype = SQL_C_BINARY;
                        b.sqltype = blob.size() > 8000 ? SQL_LONGVARBINARY : SQL_VARBINARY;
                        b.colsize = static_cast<SQLULEN>(blob.size());
                        b.len = static_cast<SQLLEN>(blob.size());
                        b.ind = static_cast<SQLLEN>(blob.size());
                        b.data = std::move(blob);
                    }
                    else if constexpr (std::is_same_v<T, date>)
                    {
                        SQL_DATE_STRUCT d {};
                        d.year = static_cast<SQLSMALLINT>(v.year);
                        d.month = static_cast<SQLUSMALLINT>(v.month);
                        d.day = static_cast<SQLUSMALLINT>(v.day);
                        b.ctype = SQL_C_TYPE_DATE;
                        b.sqltype = SQL_TYPE_DATE;
                        b.len = static_cast<SQLLEN>(sizeof(d));
                        b.ind = static_cast<SQLLEN>(sizeof(d));
                        b.data = d;
                    }
                    else if constexpr (std::is_same_v<T, datetime>)
                    {
                        SQL_TIMESTAMP_STRUCT t {};
                        t.year = static_cast<SQLSMALLINT>(v.year);
                        t.month = static_cast<SQLUSMALLINT>(v.month);
                        t.day = static_cast<SQLUSMALLINT>(v.day);
                        t.hour = static_cast<SQLUSMALLINT>(v.hour);
                        t.minute = static_cast<SQLUSMALLINT>(v.minute);
                        t.second = static_cast<SQLUSMALLINT>(v.second);
                        t.fraction = static_cast<SQLUINTEGER>(v.microsecond * 1000);
                        b.ctype = SQL_C_TYPE_TIMESTAMP;
                        b.sqltype = SQL_TYPE_TIMESTAMP;
                        b.colsize = 19;
                        b.dec = 6; ///< ColumnSize=19 是标准时间戳精度，小数位（scale）由 dec 声明。
                        b.len = static_cast<SQLLEN>(sizeof(t));
                        b.ind = static_cast<SQLLEN>(sizeof(t));
                        b.data = t;
                    }
                    else if constexpr (std::is_same_v<T, time>)
                    {
                        SQL_TIME_STRUCT t {};
                        t.hour = static_cast<SQLUSMALLINT>(v.hour);
                        t.minute = static_cast<SQLUSMALLINT>(v.minute);
                        t.second = static_cast<SQLUSMALLINT>(v.second);
                        b.ctype = SQL_C_TYPE_TIME;
                        b.sqltype = SQL_TYPE_TIME;
                        b.colsize = 7;
                        b.len = static_cast<SQLLEN>(sizeof(t));
                        b.ind = static_cast<SQLLEN>(sizeof(t));
                        b.data = t;
                    }
                    else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>)
                    {
                        // time_point（UTC）按墙上时钟存文本，语义与 SQLite 后端一致。
                        auto dt = datetime::from_time_point(v);
                        SQL_TIMESTAMP_STRUCT t {};
                        t.year = static_cast<SQLSMALLINT>(dt.year);
                        t.month = static_cast<SQLUSMALLINT>(dt.month);
                        t.day = static_cast<SQLUSMALLINT>(dt.day);
                        t.hour = static_cast<SQLUSMALLINT>(dt.hour);
                        t.minute = static_cast<SQLUSMALLINT>(dt.minute);
                        t.second = static_cast<SQLUSMALLINT>(dt.second);
                        t.fraction = static_cast<SQLUINTEGER>(dt.microsecond * 1000);
                        b.ctype = SQL_C_TYPE_TIMESTAMP;
                        b.sqltype = SQL_TYPE_TIMESTAMP;
                        b.colsize = 19;
                        b.dec = 6;
                        b.len = static_cast<SQLLEN>(sizeof(t));
                        b.ind = static_cast<SQLLEN>(sizeof(t));
                        b.data = t;
                    }
                    else if constexpr (std::is_same_v<T, param_array>)
                    {
                        // 数组参数已在渲染层展开，不应到达后端。
                        throw db_exception(boost::system::error_code {}, "db: array parameter not expanded");
                    }
                },
                p);
            return b;
        }

        db::column_type
        map_sql_type(SQLSMALLINT t)
        {
            switch (t)
            {
                case SQL_TINYINT:
                case SQL_SMALLINT:
                case SQL_INTEGER:
                case SQL_BIGINT:
                case SQL_BIT:
                    return db::column_type::int64;
                case SQL_REAL:
                case SQL_FLOAT:
                case SQL_DOUBLE:
                case SQL_DECIMAL:
                case SQL_NUMERIC:
                    return db::column_type::double_;
                case SQL_CHAR:
                case SQL_VARCHAR:
                case SQL_LONGVARCHAR:
                case SQL_WCHAR:
                case SQL_WVARCHAR:
                case SQL_WLONGVARCHAR:
                case SQL_GUID:
                    return db::column_type::string;
                case SQL_BINARY:
                case SQL_VARBINARY:
                case SQL_LONGVARBINARY:
                    return db::column_type::blob;
                case SQL_TYPE_DATE:
                    return db::column_type::date;
                case SQL_TYPE_TIMESTAMP:
                case SQL_TYPE_UTCDATETIME:
                case SQL_SS_TIMESTAMP2:
                case SQL_SS_TIMESTAMPOFFSET:
                    return db::column_type::datetime;
                case SQL_TYPE_TIME:
                case SQL_SS_TIME2:
                    return db::column_type::time;
                default:
                    return db::column_type::unknown;
            }
        }

        // 读可变长文本（SQL_C_CHAR）；先取长度再分块取数据。
        std::string
        read_text(SQLHSTMT stmt, SQLSMALLINT col, bool& is_null)
        {
            SQLLEN ind = 0;
            SQLRETURN rc = SQLGetData(stmt, col, SQL_C_CHAR, nullptr, 0, &ind);
            if (ind == SQL_NULL_DATA)
            {
                is_null = true;
                return {};
            }
            is_null = false;
            std::string out;
            if (ind > 0 && ind != SQL_NO_TOTAL)
            {
                out.reserve(static_cast<size_t>(ind) + 1);
            }
            char buf[4096];
            while (true)
            {
                rc = SQLGetData(stmt, col, SQL_C_CHAR, buf, static_cast<SQLLEN>(sizeof(buf)), &ind);
                if (rc == SQL_NO_DATA)
                {
                    break;
                }
                if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
                {
                    throw db_exception(boost::system::error_code {},
                                       "db: odbc read text: " + odbc_error_text(stmt, SQL_HANDLE_STMT));
                }
                if (ind == SQL_NULL_DATA)
                {
                    break;
                }
                // SQL Server (MAX) 类型在 SQL_SUCCESS_WITH_INFO 时 ind 是剩余字节数，
                // 此时缓冲区必然已填满；仅 SQL_SUCCESS（末块）时 ind 才是本块实际长度。
                size_t n = rc == SQL_SUCCESS ? static_cast<size_t>(ind) : sizeof(buf);
                out.append(buf, n);
                if (rc == SQL_SUCCESS)
                {
                    break;
                }
            }
            return out;
        }

        // 读可变长二进制（SQL_C_BINARY）。
        std::vector<std::byte>
        read_blob(SQLHSTMT stmt, SQLSMALLINT col, bool& is_null)
        {
            SQLLEN ind = 0;
            SQLRETURN rc = SQLGetData(stmt, col, SQL_C_BINARY, nullptr, 0, &ind);
            if (ind == SQL_NULL_DATA)
            {
                is_null = true;
                return {};
            }
            is_null = false;
            std::vector<std::byte> out;
            if (ind > 0 && ind != SQL_NO_TOTAL)
            {
                out.reserve(static_cast<size_t>(ind));
            }
            std::byte buf[4096];
            while (true)
            {
                rc = SQLGetData(stmt, col, SQL_C_BINARY, buf, static_cast<SQLLEN>(sizeof(buf)), &ind);
                if (rc == SQL_NO_DATA)
                {
                    break;
                }
                if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
                {
                    throw db_exception(boost::system::error_code {},
                                       "db: odbc read blob: " + odbc_error_text(stmt, SQL_HANDLE_STMT));
                }
                if (ind == SQL_NULL_DATA)
                {
                    break;
                }
                // SQL Server (MAX) 类型在 SQL_SUCCESS_WITH_INFO 时 ind 是剩余字节数，
                // 此时缓冲区必然已填满；仅 SQL_SUCCESS（末块）时 ind 才是本块实际长度。
                size_t n = rc == SQL_SUCCESS ? static_cast<size_t>(ind) : sizeof(buf);
                out.insert(out.end(), buf, buf + n);
                if (rc == SQL_SUCCESS)
                {
                    break;
                }
            }
            return out;
        }

        // 解析 DECIMAL 字符串：整数形式优先 int64/uint64，否则 double。
        field
        parse_decimal(std::string const& sv)
        {
            if (!sv.empty() && sv.find('.') == std::string::npos && sv.find('e') == std::string::npos
                && sv.find('E') == std::string::npos)
            {
                int64_t ll = 0;
                auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), ll);
                if (ec == std::errc {} && ptr == sv.data() + sv.size())
                {
                    return ll;
                }
                uint64_t ull = 0;
                auto [ptr2, ec2] = std::from_chars(sv.data(), sv.data() + sv.size(), ull);
                if (ec2 == std::errc {} && ptr2 == sv.data() + sv.size())
                {
                    return ull;
                }
            }
            double d = 0;
            auto [ptr3, ec3] = std::from_chars(sv.data(), sv.data() + sv.size(), d);
            if (ec3 == std::errc {} && ptr3 == sv.data() + sv.size())
            {
                return d;
            }
            throw std::runtime_error("db: cannot parse DECIMAL value: " + sv);
        }

        // 读单个字段。
        field
        read_field(SQLHSTMT stmt, SQLSMALLINT col, db::column_type ct)
        {
            SQLLEN ind = 0;
            SQLRETURN rc = SQL_SUCCESS;
            switch (ct)
            {
                case db::column_type::int64:
                {
                    SQLBIGINT v = 0;
                    rc = SQLGetData(stmt, col, SQL_C_SBIGINT, &v, sizeof(v), &ind);
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read int64");
                    if (ind == SQL_NULL_DATA)
                    {
                        return std::monostate {};
                    }
                    return static_cast<int64_t>(v);
                }
                case db::column_type::uint64:
                {
                    SQLUBIGINT v = 0;
                    rc = SQLGetData(stmt, col, SQL_C_UBIGINT, &v, sizeof(v), &ind);
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read uint64");
                    if (ind == SQL_NULL_DATA)
                    {
                        return std::monostate {};
                    }
                    return static_cast<uint64_t>(v);
                }
                case db::column_type::double_:
                {
                    bool null = false;
                    auto sv = read_text(stmt, col, null);
                    if (null)
                    {
                        return std::monostate {};
                    }
                    return parse_decimal(sv);
                }
                case db::column_type::blob:
                {
                    bool null = false;
                    auto b = read_blob(stmt, col, null);
                    if (null)
                    {
                        return std::monostate {};
                    }
                    return b;
                }
                case db::column_type::date:
                {
                    SQL_DATE_STRUCT d {};
                    rc = SQLGetData(stmt, col, SQL_C_TYPE_DATE, &d, sizeof(d), &ind);
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read date");
                    if (ind == SQL_NULL_DATA)
                    {
                        return std::monostate {};
                    }
                    return date { static_cast<unsigned>(d.year),
                                  static_cast<unsigned>(d.month),
                                  static_cast<unsigned>(d.day) };
                }
                case db::column_type::datetime:
                {
                    SQL_TIMESTAMP_STRUCT t {};
                    rc = SQLGetData(stmt, col, SQL_C_TYPE_TIMESTAMP, &t, sizeof(t), &ind);
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read datetime");
                    if (ind == SQL_NULL_DATA)
                    {
                        return std::monostate {};
                    }
                    return datetime { static_cast<unsigned>(t.year),
                                      static_cast<unsigned>(t.month),
                                      static_cast<unsigned>(t.day),
                                      static_cast<unsigned>(t.hour),
                                      static_cast<unsigned>(t.minute),
                                      static_cast<unsigned>(t.second),
                                      static_cast<unsigned long>(t.fraction / 1000) };
                }
                case db::column_type::time:
                {
                    SQL_TIME_STRUCT t {};
                    rc = SQLGetData(stmt, col, SQL_C_TYPE_TIME, &t, sizeof(t), &ind);
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read time");
                    if (ind == SQL_NULL_DATA)
                    {
                        return std::monostate {};
                    }
                    return time { static_cast<unsigned>(t.hour),
                                  static_cast<unsigned>(t.minute),
                                  static_cast<unsigned>(t.second),
                                  0 };
                }
                default:
                {
                    bool null = false;
                    auto sv = read_text(stmt, col, null);
                    if (null)
                    {
                        return std::monostate {};
                    }
                    return std::string(std::move(sv));
                }
            }
        }

        // 读取语句全部结果集（含多结果集）。
        result
        read_all(SQLHSTMT stmt)
        {
            std::vector<result::resultset> sets;
            while (true)
            {
                result::resultset s;
                SQLSMALLINT ncols = 0;
                SQLRETURN rc = SQLNumResultCols(stmt, &ncols);
                check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc num result cols");

                SQLLEN affected = 0;
                rc = SQLRowCount(stmt, &affected);
                if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
                {
                    s.affected = affected > 0 ? static_cast<uint64_t>(affected) : 0;
                }

                if (ncols > 0)
                {
                    s.names.reserve(static_cast<size_t>(ncols));
                    s.types.reserve(static_cast<size_t>(ncols));
                    for (SQLSMALLINT i = 1; i <= ncols; ++i)
                    {
                        SQLCHAR name[1024] = {};
                        SQLSMALLINT name_len = 0;
                        SQLSMALLINT sqltype = 0;
                        SQLULEN colsize = 0;
                        SQLSMALLINT dec = 0;
                        SQLSMALLINT nullable = 0;
                        rc = SQLDescribeCol(stmt,
                                            i,
                                            name,
                                            static_cast<SQLSMALLINT>(sizeof(name)),
                                            &name_len,
                                            &sqltype,
                                            &colsize,
                                            &dec,
                                            &nullable);
                        check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc describe col");
                        s.names.emplace_back(reinterpret_cast<char*>(name), static_cast<size_t>(name_len));
                        s.types.push_back(map_sql_type(sqltype));
                    }

                    while (SQLFetch(stmt) == SQL_SUCCESS)
                    {
                        std::vector<field> values;
                        values.reserve(static_cast<size_t>(ncols));
                        for (SQLSMALLINT i = 1; i <= ncols; ++i)
                        {
                            values.push_back(read_field(stmt, i, s.types[static_cast<size_t>(i - 1)]));
                        }
                        s.rows.push_back(std::move(values));
                    }
                }
                sets.push_back(std::move(s));

                rc = SQLMoreResults(stmt);
                if (rc == SQL_NO_DATA)
                {
                    break;
                }
                check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc more results");
            }
            return result(std::move(sets), std::chrono::seconds { 0 });
        }

        SQLHSTMT
        alloc_statement(SQLHDBC dbc)
        {
            SQLHSTMT stmt = nullptr;
            SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
            check_ok(rc, dbc, SQL_HANDLE_DBC, "odbc alloc statement");
            return stmt;
        }
    } // namespace

    odbc_backend::odbc_backend(odbc_config cfg) : cfg_(std::move(cfg)) {}

    odbc_backend::~odbc_backend() { disconnect(); }

    void
    odbc_backend::disconnect() noexcept
    {
        if (dbc_)
        {
            SQLDisconnect(static_cast<SQLHDBC>(dbc_));
            SQLFreeHandle(SQL_HANDLE_DBC, dbc_);
            dbc_ = nullptr;
        }
        if (env_)
        {
            SQLFreeHandle(SQL_HANDLE_ENV, env_);
            env_ = nullptr;
        }
    }

    net::awaitable<void>
    odbc_backend::connect()
    {
        SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_);
        check_ok(rc, env_, SQL_HANDLE_ENV, "odbc alloc env");
        rc = SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
        check_ok(rc, env_, SQL_HANDLE_ENV, "odbc set version");
        rc = SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc_);
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc alloc dbc");

        if (!cfg_.connection_string.empty())
        {
            SQLCHAR out[1024] = {};
            SQLSMALLINT out_len = 0;
            std::string cs(cfg_.connection_string);
            rc = SQLDriverConnect(dbc_,
                                  nullptr,
                                  reinterpret_cast<SQLCHAR*>(cs.data()),
                                  static_cast<SQLSMALLINT>(cs.size()),
                                  out,
                                  static_cast<SQLSMALLINT>(sizeof(out)),
                                  &out_len,
                                  SQL_DRIVER_NOPROMPT);
            check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc connect (connection string)");
        }
        else
        {
            std::string dsn(cfg_.dsn);
            std::string user(cfg_.user);
            std::string pwd(cfg_.password);
            rc = SQLConnect(dbc_,
                            reinterpret_cast<SQLCHAR*>(dsn.data()),
                            static_cast<SQLSMALLINT>(dsn.size()),
                            reinterpret_cast<SQLCHAR*>(user.data()),
                            static_cast<SQLSMALLINT>(user.size()),
                            reinterpret_cast<SQLCHAR*>(pwd.data()),
                            static_cast<SQLSMALLINT>(pwd.size()));
            check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc connect (dsn)");
        }
        co_return;
    }

    net::awaitable<void>
    odbc_backend::reconnect()
    {
        disconnect();
        co_await connect();
    }

    net::awaitable<bool>
    odbc_backend::ping()
    {
        if (!dbc_)
        {
            co_return false;
        }
        try
        {
            SQLHSTMT stmt = alloc_statement(static_cast<SQLHDBC>(dbc_));
            SQLCHAR sql[] = "SELECT 1";
            SQLRETURN rc = SQLExecDirect(stmt, sql, SQL_NTS);
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
            {
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                co_return false;
            }
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            co_return true;
        }
        catch (...)
        {
            co_return false;
        }
    }

    result
    odbc_backend::exec(std::string_view sql, std::vector<param> const& params)
    {
        if (!dbc_)
        {
            throw db_exception(boost::system::error_code {}, "db: odbc not connected");
        }
        SQLHDBC dbc = static_cast<SQLHDBC>(dbc_);
        SQLHSTMT stmt = alloc_statement(dbc);

        try
        {
            if (params.empty())
            {
                std::string sql_str(sql);
                SQLRETURN rc = SQLExecDirect(stmt,
                                             reinterpret_cast<SQLCHAR*>(sql_str.data()),
                                             static_cast<SQLINTEGER>(sql_str.size()));
                check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc execute");
            }
            else
            {
                std::string sql_str(sql);
                SQLRETURN rc = SQLPrepare(stmt,
                                          reinterpret_cast<SQLCHAR*>(sql_str.data()),
                                          static_cast<SQLINTEGER>(sql_str.size()));
                check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc prepare");

                std::vector<odbc_bind> binds;
                binds.reserve(params.size());
                for (auto const& p : params)
                {
                    binds.push_back(make_bind(p));
                }
                for (size_t i = 0; i < binds.size(); ++i)
                {
                    rc = SQLBindParameter(stmt,
                                          static_cast<SQLUSMALLINT>(i + 1),
                                          SQL_PARAM_INPUT,
                                          binds[i].ctype,
                                          binds[i].sqltype,
                                          binds[i].colsize,
                                          binds[i].dec,
                                          binds[i].ptr(),
                                          binds[i].len,
                                          &binds[i].ind);
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc bind parameter");
                }
                rc = SQLExecute(stmt);
                check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc execute statement");
            }
            result r = read_all(stmt);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return r;
        }
        catch (...)
        {
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            throw;
        }
    }

    net::awaitable<result>
    odbc_backend::execute(std::string_view sql)
    {
        co_return exec(sql, {});
    }

    net::awaitable<statement_handle>
    odbc_backend::prepare(std::string_view sql)
    {
        if (!dbc_)
        {
            throw db_exception(boost::system::error_code {}, "db: odbc not connected");
        }
        SQLHDBC dbc = static_cast<SQLHDBC>(dbc_);
        SQLHSTMT stmt = alloc_statement(dbc);
        std::string sql_str(sql);
        SQLRETURN rc
            = SQLPrepare(stmt, reinterpret_cast<SQLCHAR*>(sql_str.data()), static_cast<SQLINTEGER>(sql_str.size()));
        check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc prepare");
        co_return statement_handle { stmt };
    }

    net::awaitable<result>
    odbc_backend::execute_statement(statement_handle h, std::vector<param> const& params)
    {
        auto* stmt = static_cast<SQLHSTMT>(h.state);
        if (!stmt)
        {
            throw db_exception(boost::system::error_code {}, "db: odbc statement not prepared");
        }

        // 复用语句：关闭游标并清除上一轮绑定。
        SQLFreeStmt(stmt, SQL_CLOSE);
        SQLFreeStmt(stmt, SQL_RESET_PARAMS);

        std::vector<odbc_bind> binds;
        binds.reserve(params.size());
        for (auto const& p : params)
        {
            binds.push_back(make_bind(p));
        }
        for (size_t i = 0; i < binds.size(); ++i)
        {
            SQLRETURN rc = SQLBindParameter(stmt,
                                            static_cast<SQLUSMALLINT>(i + 1),
                                            SQL_PARAM_INPUT,
                                            binds[i].ctype,
                                            binds[i].sqltype,
                                            binds[i].colsize,
                                            binds[i].dec,
                                            binds[i].ptr(),
                                            binds[i].len,
                                            &binds[i].ind);
            check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc bind parameter");
        }
        SQLRETURN rc = SQLExecute(stmt);
        check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc execute statement");
        co_return read_all(stmt);
    }

    net::awaitable<void>
    odbc_backend::close_statement(statement_handle h) noexcept
    {
        if (h.state)
        {
            SQLFreeHandle(SQL_HANDLE_STMT, static_cast<SQLHSTMT>(h.state));
        }
        co_return;
    }

    net::awaitable<void>
    odbc_backend::begin()
    {
        SQLRETURN rc = SQLSetConnectAttr(static_cast<SQLHDBC>(dbc_),
                                         SQL_ATTR_AUTOCOMMIT,
                                         reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF),
                                         SQL_IS_UINTEGER);
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc begin");
        co_return;
    }

    net::awaitable<void>
    odbc_backend::commit()
    {
        SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc_), SQL_COMMIT);
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc commit");
        rc = SQLSetConnectAttr(static_cast<SQLHDBC>(dbc_),
                               SQL_ATTR_AUTOCOMMIT,
                               reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON),
                               SQL_IS_UINTEGER);
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc autocommit restore");
        co_return;
    }

    net::awaitable<void>
    odbc_backend::rollback()
    {
        SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc_), SQL_ROLLBACK);
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc rollback");
        rc = SQLSetConnectAttr(static_cast<SQLHDBC>(dbc_),
                               SQL_ATTR_AUTOCOMMIT,
                               reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON),
                               SQL_IS_UINTEGER);
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc autocommit restore");
        co_return;
    }

    void
    register_odbc_backend()
    {
        register_backend("odbc",
                         [](net::any_io_executor ex, options const& opts) -> std::unique_ptr<backend>
                         {
                             (void)ex;
                             odbc_config cfg;
                             cfg.connection_string = opts.get_or("connection_string", cfg.connection_string);
                             cfg.dsn = opts.get_or("dsn", cfg.dsn);
                             cfg.user = opts.get_or("uid", cfg.user);
                             cfg.password = opts.get_or("pwd", cfg.password);
                             return std::make_unique<odbc_backend>(std::move(cfg));
                         });
    }

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE