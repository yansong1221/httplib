#ifdef HTTPLIB_ENABLED_DATABASE
#include "odbc_backend.hpp"
#include "httplib/db/exception.hpp"
#include "registry.hpp"
#include <sql.h>
#include <sqlext.h>
#include <windows.h>
// SQL Server 专用类型（msodbcsql.h 里的 -150 ~ -199 保留段；此处只 include 了 sql.h/sqlext.h，
// 故手动声明与驱动一致的常量值）。注意 DATETIMEOFFSET 是 -155，不是 -140。
#ifndef SQL_SS_TIMESTAMPOFFSET
#define SQL_SS_TIMESTAMPOFFSET (-155)
#endif
#ifndef SQL_SS_TIME2
#define SQL_SS_TIME2 (-154)
#endif
#ifndef SQL_SS_TIME2_STRUCT
// SQL Server 专用 TIME2 结构（sqltypes.h 不含）；fraction 单位纳秒。
struct SQL_SS_TIME2_STRUCT
{
    SQLUSMALLINT hour;
    SQLUSMALLINT minute;
    SQLUSMALLINT second;
    SQLUINTEGER fraction;
};
#endif
#ifndef SQL_SS_TIMESTAMPOFFSET_STRUCT
// SQL Server 专用 DATETIMEOFFSET 结构（sqltypes.h 不含）；fraction 单位纳秒，
// timezone_hour/timezone_minute 为偏移量。用 SQL_C_BINARY 读取可保留原始墙上时钟与偏移。
struct SQL_SS_TIMESTAMPOFFSET_STRUCT
{
    SQLSMALLINT year;
    SQLUSMALLINT month;
    SQLUSMALLINT day;
    SQLUSMALLINT hour;
    SQLUSMALLINT minute;
    SQLUSMALLINT second;
    SQLUINTEGER fraction;
    SQLSMALLINT timezone_hour;
    SQLSMALLINT timezone_minute;
};
#endif
#include <charconv>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace httplib::db::detail
{
    namespace
    {
        // 获取 ODBC 错误文本（首个诊断记录），供异常消息使用。
        static std::string
        odbc_error_text(SQLHANDLE handle, SQLSMALLINT handle_type)
        {
            SQLCHAR state[6] = {};
            SQLCHAR msg[1024] = {};
            SQLSMALLINT msg_len = 0;
            SQLINTEGER native = 0;
            SQLGetDiagRec(handle_type, handle, 1, state, &native, msg, static_cast<SQLSMALLINT>(sizeof(msg)), &msg_len);
            // msg_len 是"完整消息长度"，某些驱动会报告超过缓冲的实际长度；
            // 必须钳制到缓冲大小，否则 std::string(msg, msg_len) 栈越界读。
            auto len = static_cast<size_t>(std::min<SQLSMALLINT>(msg_len, static_cast<SQLSMALLINT>(sizeof(msg) - 1)));
            return std::string("odbc error [") + reinterpret_cast<char*>(state) + "] "
                   + std::string(reinterpret_cast<char*>(msg), len) + " (native " + std::to_string(native) + ")";
        }

        /// 诊断记录是否属于连接异常类（SQLSTATE 以 "08" 开头）。
        static bool
        odbc_connection_lost(SQLHANDLE handle, SQLSMALLINT handle_type)
        {
            SQLCHAR state[6] = {};
            SQLGetDiagRec(handle_type, handle, 1, state, nullptr, nullptr, 0, nullptr);
            return state[0] == '0' && state[1] == '8';
        }

        static void
        check_ok(SQLRETURN rc, SQLHANDLE handle, SQLSMALLINT handle_type, std::string_view what)
        {
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
            {
                throw db_exception(boost::system::error_code {},
                                   "db: " + std::string(what) + ": " + odbc_error_text(handle, handle_type));
            }
        }

        /// 调用一个 ODBC 函数；若返回 SQL_STILL_EXECUTING 则等待关联事件并 SQLCompleteAsync，
        /// 直到取到最终返回码。fn 只调用一次。
        template <typename F>
        net::awaitable<SQLRETURN>
        odbc_async(SQLHANDLE handle, SQLSMALLINT handle_type, net::windows::object_handle& obj, F&& fn)
        {
            SQLRETURN rc = fn();
            while (rc == SQL_STILL_EXECUTING)
            {
                co_await obj.async_wait(net::use_awaitable);
                SQLRETURN r = SQL_SUCCESS;
                SQLCompleteAsync(handle_type, handle, &r);
                rc = r;
            }
            co_return rc;
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
                         SQL_TIMESTAMP_STRUCT,
                         SQL_SS_TIME2_STRUCT>
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

        static odbc_bind
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
                        b.sqltype = v.size() > 8000 ? SQL_LONGVARCHAR : SQL_VARCHAR;
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
                        // SQL_SS_TIME2 + SQL_C_BINARY 才能携带小数秒；
                        // 旧实现用 SQL_C_TYPE_TIME/SQL_TIME_STRUCT，fraction 恒为 0。
                        SQL_SS_TIME2_STRUCT t {};
                        t.hour = static_cast<SQLUSMALLINT>(v.hour);
                        t.minute = static_cast<SQLUSMALLINT>(v.minute);
                        t.second = static_cast<SQLUSMALLINT>(v.second);
                        t.fraction = static_cast<SQLUINTEGER>(v.microsecond * 1000); ///< 纳秒
                        b.ctype = SQL_C_BINARY;
                        b.sqltype = SQL_SS_TIME2;
                        b.colsize = 16;
                        b.dec = 7;
                        b.len = static_cast<SQLLEN>(sizeof(t));
                        b.ind = static_cast<SQLLEN>(sizeof(t));
                        b.data = t;
                    }
                    else if constexpr (std::is_same_v<T, timestamp>)
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
                },
                p);
            return b;
        }

        static db::column_type
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
        static net::awaitable<std::string>
        read_text(SQLHSTMT stmt, net::windows::object_handle& obj, SQLSMALLINT col, bool& is_null)
        {
            // 用真实小缓冲探测：NULL 列在本驱动下以 SQL_NULL_DATA 报告（nullptr/0 探测会 SQL_ERROR）。
            SQLLEN ind = 0;
            char probe[256];
            SQLRETURN rc
                = co_await odbc_async(stmt,
                                      SQL_HANDLE_STMT,
                                      obj,
                                      [&] { return SQLGetData(stmt, col, SQL_C_CHAR, probe, sizeof(probe), &ind); });
            if (rc == SQL_NO_DATA || ind == SQL_NULL_DATA)
            {
                is_null = true;
                co_return std::string {};
            }
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
            {
                throw db_exception(boost::system::error_code {},
                                   "db: odbc read text: " + odbc_error_text(stmt, SQL_HANDLE_STMT));
            }
            is_null = false;
            std::string out;
            if (ind > 0 && ind != SQL_NO_TOTAL)
            {
                out.reserve(static_cast<size_t>(ind) + 1);
            }
            if (rc == SQL_SUCCESS)
            {
                // 整个值已放进探测缓冲（字符数据带 null 终止位，长度取 ind）。
                out.append(probe, static_cast<size_t>(ind));
                co_return out;
            }
            // 值超出一个缓冲：探测缓冲已填满（字符数据最多 bufferlen-1，留 null 终止位）。
            out.append(probe, sizeof(probe) - 1);
            char buf[4096];
            while (true)
            {
                rc = co_await odbc_async(stmt,
                                         SQL_HANDLE_STMT,
                                         obj,
                                         [&] { return SQLGetData(stmt, col, SQL_C_CHAR, buf, sizeof(buf), &ind); });
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
                // 此时缓冲区必然已填满（字符数据最多 bufferlen-1，留 null 终止位），
                // 仅 SQL_SUCCESS（末块）时 ind 才是本块实际长度。
                size_t n = rc == SQL_SUCCESS ? static_cast<size_t>(ind) : sizeof(buf) - 1;
                out.append(buf, n);
                if (rc == SQL_SUCCESS)
                {
                    break;
                }
            }
            co_return out;
        }

        // 读可变长二进制（SQL_C_BINARY）。
        static net::awaitable<std::vector<std::byte>>
        read_blob(SQLHSTMT stmt, net::windows::object_handle& obj, SQLSMALLINT col, bool& is_null)
        {
            // 用真实小缓冲探测：NULL 列在本驱动下以 SQL_NULL_DATA 报告（nullptr/0 探测会 SQL_ERROR）。
            SQLLEN ind = 0;
            std::byte probe[256];
            SQLRETURN rc
                = co_await odbc_async(stmt,
                                      SQL_HANDLE_STMT,
                                      obj,
                                      [&] { return SQLGetData(stmt, col, SQL_C_BINARY, probe, sizeof(probe), &ind); });
            if (rc == SQL_NO_DATA || ind == SQL_NULL_DATA)
            {
                is_null = true;
                co_return std::vector<std::byte> {};
            }
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
            {
                throw db_exception(boost::system::error_code {},
                                   "db: odbc read blob: " + odbc_error_text(stmt, SQL_HANDLE_STMT));
            }
            is_null = false;
            std::vector<std::byte> out;
            if (ind > 0 && ind != SQL_NO_TOTAL)
            {
                out.reserve(static_cast<size_t>(ind));
            }
            if (rc == SQL_SUCCESS)
            {
                out.insert(out.end(), probe, probe + static_cast<size_t>(ind));
                co_return out;
            }
            out.insert(out.end(), probe, probe + sizeof(probe));
            std::byte buf[4096];
            while (true)
            {
                rc = co_await odbc_async(stmt,
                                         SQL_HANDLE_STMT,
                                         obj,
                                         [&] { return SQLGetData(stmt, col, SQL_C_BINARY, buf, sizeof(buf), &ind); });
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
            co_return out;
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
        net::awaitable<field>
        read_field(SQLHSTMT stmt,
                   net::windows::object_handle& obj,
                   SQLSMALLINT col,
                   db::column_type ct,
                   SQLSMALLINT sqltype)
        {
            SQLLEN ind = 0;
            SQLRETURN rc = SQL_SUCCESS;
            if (sqltype == SQL_GUID)
            {
                // uniqueidentifier 必须用 SQL_C_GUID 读取，NULL 才会正确报告 SQL_NULL_DATA
                // （SQL_C_CHAR 会把 NULL 读成空串）。
                SQLGUID g {};
                rc = co_await odbc_async(stmt,
                                         SQL_HANDLE_STMT,
                                         obj,
                                         [&] { return SQLGetData(stmt, col, SQL_C_GUID, &g, sizeof(g), &ind); });
                check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read guid");
                if (ind == SQL_NULL_DATA)
                {
                    co_return std::monostate {};
                }
                char buf[37];
                std::snprintf(buf,
                              sizeof(buf),
                              "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                              g.Data1,
                              g.Data2,
                              g.Data3,
                              g.Data4[0],
                              g.Data4[1],
                              g.Data4[2],
                              g.Data4[3],
                              g.Data4[4],
                              g.Data4[5],
                              g.Data4[6],
                              g.Data4[7]);
                co_return std::string(buf);
            }
            switch (ct)
            {
                case db::column_type::int64:
                {
                    SQLBIGINT v = 0;
                    rc = co_await odbc_async(stmt,
                                             SQL_HANDLE_STMT,
                                             obj,
                                             [&] { return SQLGetData(stmt, col, SQL_C_SBIGINT, &v, sizeof(v), &ind); });
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read int64");
                    if (ind == SQL_NULL_DATA)
                    {
                        co_return std::monostate {};
                    }
                    co_return static_cast<int64_t>(v);
                }
                case db::column_type::uint64:
                {
                    SQLUBIGINT v = 0;
                    rc = co_await odbc_async(stmt,
                                             SQL_HANDLE_STMT,
                                             obj,
                                             [&] { return SQLGetData(stmt, col, SQL_C_UBIGINT, &v, sizeof(v), &ind); });
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read uint64");
                    if (ind == SQL_NULL_DATA)
                    {
                        co_return std::monostate {};
                    }
                    co_return static_cast<uint64_t>(v);
                }
                case db::column_type::double_:
                {
                    bool null = false;
                    auto sv = co_await read_text(stmt, obj, col, null);
                    if (null)
                    {
                        co_return std::monostate {};
                    }
                    co_return parse_decimal(sv);
                }
                case db::column_type::blob:
                {
                    bool null = false;
                    auto b = co_await read_blob(stmt, obj, col, null);
                    if (null)
                    {
                        co_return std::monostate {};
                    }
                    co_return b;
                }
                case db::column_type::date:
                {
                    SQL_DATE_STRUCT d {};
                    rc = co_await odbc_async(stmt,
                                             SQL_HANDLE_STMT,
                                             obj,
                                             [&]
                                             { return SQLGetData(stmt, col, SQL_C_TYPE_DATE, &d, sizeof(d), &ind); });
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read date");
                    if (ind == SQL_NULL_DATA)
                    {
                        co_return std::monostate {};
                    }
                    co_return date { static_cast<unsigned>(d.year),
                                     static_cast<unsigned>(d.month),
                                     static_cast<unsigned>(d.day) };
                }
                case db::column_type::datetime:
                {
                    if (sqltype == SQL_SS_TIMESTAMPOFFSET)
                    {
                        // DATETIMEOFFSET 用 SQL_C_BINARY 取 SQL_SS_TIMESTAMPOFFSET_STRUCT，
                        // 保留原始墙上时钟；若用 SQL_C_TYPE_TIMESTAMP 读取，驱动会按会话时区
                        // 换算并丢弃偏移，墙上时钟往返失真。
                        SQL_SS_TIMESTAMPOFFSET_STRUCT t {};
                        rc = co_await odbc_async(stmt,
                                                 SQL_HANDLE_STMT,
                                                 obj,
                                                 [&]
                                                 { return SQLGetData(stmt, col, SQL_C_BINARY, &t, sizeof(t), &ind); });
                        check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read datetimeoffset");
                        if (ind == SQL_NULL_DATA)
                        {
                            co_return std::monostate {};
                        }
                        co_return datetime { static_cast<unsigned>(t.year),
                                             static_cast<unsigned>(t.month),
                                             static_cast<unsigned>(t.day),
                                             static_cast<unsigned>(t.hour),
                                             static_cast<unsigned>(t.minute),
                                             static_cast<unsigned>(t.second),
                                             static_cast<unsigned long>(t.fraction / 1000) };
                    }
                    SQL_TIMESTAMP_STRUCT t {};
                    rc = co_await odbc_async(
                        stmt,
                        SQL_HANDLE_STMT,
                        obj,
                        [&] { return SQLGetData(stmt, col, SQL_C_TYPE_TIMESTAMP, &t, sizeof(t), &ind); });
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read datetime");
                    if (ind == SQL_NULL_DATA)
                    {
                        co_return std::monostate {};
                    }
                    co_return datetime { static_cast<unsigned>(t.year),
                                         static_cast<unsigned>(t.month),
                                         static_cast<unsigned>(t.day),
                                         static_cast<unsigned>(t.hour),
                                         static_cast<unsigned>(t.minute),
                                         static_cast<unsigned>(t.second),
                                         static_cast<unsigned long>(t.fraction / 1000) };
                }
                case db::column_type::time:
                {
                    if (sqltype == SQL_SS_TIME2)
                    {
                        // SQL Server TIME → SQL_SS_TIME2：用 SQL_C_BINARY 取 SQL_SS_TIME2_STRUCT 以保留小数秒。
                        SQL_SS_TIME2_STRUCT t {};
                        rc = co_await odbc_async(stmt,
                                                 SQL_HANDLE_STMT,
                                                 obj,
                                                 [&]
                                                 { return SQLGetData(stmt, col, SQL_C_BINARY, &t, sizeof(t), &ind); });
                        check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read time2");
                        if (ind == SQL_NULL_DATA)
                        {
                            co_return std::monostate {};
                        }
                        // fraction 单位纳秒 → 微秒。
                        co_return time { static_cast<unsigned>(t.hour),
                                         static_cast<unsigned>(t.minute),
                                         static_cast<unsigned>(t.second),
                                         static_cast<unsigned>(t.fraction / 1000) };
                    }
                    // 通用 SQL_TYPE_TIME（非 SQL Server）仅支持秒级精度。
                    SQL_TIME_STRUCT t {};
                    rc = co_await odbc_async(stmt,
                                             SQL_HANDLE_STMT,
                                             obj,
                                             [&]
                                             { return SQLGetData(stmt, col, SQL_C_TYPE_TIME, &t, sizeof(t), &ind); });
                    check_ok(rc, stmt, SQL_HANDLE_STMT, "odbc read time");
                    if (ind == SQL_NULL_DATA)
                    {
                        co_return std::monostate {};
                    }
                    co_return time { static_cast<unsigned>(t.hour),
                                     static_cast<unsigned>(t.minute),
                                     static_cast<unsigned>(t.second),
                                     0 };
                }
                default:
                {
                    bool null = false;
                    auto sv = co_await read_text(stmt, obj, col, null);
                    if (null)
                    {
                        co_return std::monostate {};
                    }
                    co_return std::string(std::move(sv));
                }
            }
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

    void
    odbc_backend::check_ok(SQLRETURN rc, SQLHANDLE handle, SQLSMALLINT handle_type, std::string_view what)
    {
        if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
        {
            return;
        }
        if (odbc_connection_lost(handle, handle_type))
        {
            live_ = false;
        }
        throw db_exception(boost::system::error_code {},
                           "db: " + std::string(what) + ": " + odbc_error_text(handle, handle_type));
    }

    odbc_backend::odbc_backend(net::any_io_executor ex, odbc_config cfg) : ex_(std::move(ex)), cfg_(std::move(cfg))
    {
        auto conn_event = CreateEvent(nullptr, FALSE, FALSE, nullptr); // auto-reset, 初始非信号态
        if (!conn_event)
        {
            throw db_exception(boost::system::error_code {}, "db: odbc create connection event failed");
        }
        conn_obj_ = std::make_unique<net::windows::object_handle>(ex_, static_cast<HANDLE>(conn_event));
    }

    odbc_backend::~odbc_backend()
    {
        conn_obj_.reset();
        disconnect();
    }

    void
    odbc_backend::disconnect() noexcept
    {
        if (dbc_)
        {
            // 关闭连接级异步，使 SQLDisconnect 同步完成（析构/重连路径无法 co_await）。
            SQLSetConnectAttr(dbc_,
                              SQL_ATTR_ASYNC_DBC_FUNCTIONS_ENABLE,
                              reinterpret_cast<SQLPOINTER>(SQL_ASYNC_ENABLE_OFF),
                              SQL_IS_INTEGER);
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

    void
    odbc_backend::free_stmt(odbc_stmt& s) noexcept
    {
        s.obj.reset();     // destroys windows::object_handle, which closes the HANDLE
        s.event = nullptr; // prevent double-close (obj already closed the handle)
        if (s.stmt)
        {
            SQLFreeHandle(SQL_HANDLE_STMT, s.stmt);
            s.stmt = nullptr;
        }
    }

    std::unique_ptr<odbc_backend::odbc_stmt>
    odbc_backend::new_stmt()
    {
        auto s = std::make_unique<odbc_stmt>();
        s->stmt = alloc_statement(static_cast<SQLHDBC>(dbc_));
        s->event = CreateEvent(nullptr, FALSE, FALSE, nullptr); // auto-reset, 初始非信号态
        if (!s->event)
        {
            SQLFreeHandle(SQL_HANDLE_STMT, s->stmt);
            throw db_exception(boost::system::error_code {}, "db: odbc create statement event failed");
        }
        s->obj = std::make_unique<net::windows::object_handle>(ex_, static_cast<HANDLE>(s->event));

        SQLRETURN rc = SQLSetStmtAttr(s->stmt,
                                      SQL_ATTR_ASYNC_ENABLE,
                                      reinterpret_cast<SQLPOINTER>(SQL_ASYNC_ENABLE_ON),
                                      SQL_IS_INTEGER);
        check_ok(rc, s->stmt, SQL_HANDLE_STMT, "odbc enable stmt async");
        rc = SQLSetStmtAttr(s->stmt, SQL_ATTR_ASYNC_STMT_EVENT, s->event, SQL_IS_POINTER);
        check_ok(rc, s->stmt, SQL_HANDLE_STMT, "odbc set stmt event");
        return s;
    }

    net::awaitable<SQLRETURN>
    odbc_backend::stmt_async(odbc_stmt& s, std::function<SQLRETURN()> fn)
    {
        SQLRETURN rc = fn();
        while (rc == SQL_STILL_EXECUTING)
        {
            co_await s.obj->async_wait(net::use_awaitable);
            SQLRETURN r = SQL_SUCCESS;
            SQLCompleteAsync(SQL_HANDLE_STMT, s.stmt, &r);
            rc = r;
        }
        co_return rc;
    }

    net::awaitable<SQLRETURN>
    odbc_backend::conn_async(std::function<SQLRETURN()> fn)
    {
        SQLRETURN rc = fn();
        while (rc == SQL_STILL_EXECUTING)
        {
            co_await conn_obj_->async_wait(net::use_awaitable);
            SQLRETURN r = SQL_SUCCESS;
            SQLCompleteAsync(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc_), &r);
            rc = r;
        }
        co_return rc;
    }

    net::awaitable<void>
    odbc_backend::connect()
    {
        SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_);
        check_ok(rc, env_, SQL_HANDLE_ENV, "odbc alloc env");
        rc = SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3_80), 0);
        check_ok(rc, env_, SQL_HANDLE_ENV, "odbc set version");

        rc = SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc_);
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc alloc dbc");

        // 连接级异步通知：函数执行置 ON，事件关联连接。
        rc = SQLSetConnectAttr(dbc_,
                               SQL_ATTR_ASYNC_DBC_FUNCTIONS_ENABLE,
                               reinterpret_cast<SQLPOINTER>(SQL_ASYNC_ENABLE_ON),
                               SQL_IS_INTEGER);
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc enable dbc async");
        rc = SQLSetConnectAttr(dbc_, SQL_ATTR_ASYNC_DBC_EVENT, conn_obj_->native_handle(), SQL_IS_POINTER);
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc set dbc event");

        if (!cfg_.connection_string.empty())
        {
            SQLCHAR out[1024] = {};
            SQLSMALLINT out_len = 0;
            std::string cs(cfg_.connection_string);
            rc = co_await conn_async(
                [&]
                {
                    return SQLDriverConnect(dbc_,
                                            nullptr,
                                            reinterpret_cast<SQLCHAR*>(cs.data()),
                                            static_cast<SQLSMALLINT>(cs.size()),
                                            out,
                                            static_cast<SQLSMALLINT>(sizeof(out)),
                                            &out_len,
                                            SQL_DRIVER_NOPROMPT);
                });
            check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc connect (connection string)");
        }
        else
        {
            std::string dsn(cfg_.dsn);
            std::string user(cfg_.user);
            std::string pwd(cfg_.password);
            rc = co_await conn_async(
                [&]
                {
                    return SQLConnect(dbc_,
                                      reinterpret_cast<SQLCHAR*>(dsn.data()),
                                      static_cast<SQLSMALLINT>(dsn.size()),
                                      reinterpret_cast<SQLCHAR*>(user.data()),
                                      static_cast<SQLSMALLINT>(user.size()),
                                      reinterpret_cast<SQLCHAR*>(pwd.data()),
                                      static_cast<SQLSMALLINT>(pwd.size()));
                });
            check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc connect (dsn)");
        }
        live_ = true;
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
            auto s = new_stmt();
            SQLRETURN rc = co_await stmt_async(*s,
                                               [&]
                                               {
                                                   SQLCHAR sql[] = "SELECT 1";
                                                   return SQLExecDirect(s->stmt, sql, SQL_NTS);
                                               });
            bool ok = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
            free_stmt(*s);
            if (!ok)
            {
                live_ = false;
            }
            co_return ok;
        }
        catch (...)
        {
            live_ = false;
            co_return false;
        }
    }

    net::awaitable<result>
    odbc_backend::execute(std::string_view sql)
    {
        if (!dbc_)
        {
            throw db_exception(boost::system::error_code {}, "db: odbc not connected");
        }
        auto s = new_stmt();
        std::string sql_str(sql);
        try
        {
            SQLRETURN rc = co_await stmt_async(*s,
                                               [&]
                                               {
                                                   return SQLExecDirect(s->stmt,
                                                                        reinterpret_cast<SQLCHAR*>(sql_str.data()),
                                                                        static_cast<SQLINTEGER>(sql_str.size()));
                                               });
            check_ok(rc, s->stmt, SQL_HANDLE_STMT, "odbc execute");
            result r = co_await read_all(*s);
            free_stmt(*s);
            co_return r;
        }
        catch (...)
        {
            free_stmt(*s);
            throw;
        }
    }

    net::awaitable<result>
    odbc_backend::read_all(odbc_stmt& s)
    {
        std::vector<result::resultset> sets;
        while (true)
        {
            result::resultset rs;
            SQLSMALLINT ncols = 0;
            SQLRETURN rc = SQLNumResultCols(s.stmt, &ncols);
            check_ok(rc, s.stmt, SQL_HANDLE_STMT, "odbc num result cols");

            SQLLEN affected = 0;
            rc = SQLRowCount(s.stmt, &affected);
            if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
            {
                rs.affected = affected > 0 ? static_cast<uint64_t>(affected) : 0;
            }

            if (ncols > 0)
            {
                rs.names.reserve(static_cast<size_t>(ncols));
                rs.types.reserve(static_cast<size_t>(ncols));
                std::vector<SQLSMALLINT> sqltypes;
                sqltypes.reserve(static_cast<size_t>(ncols));
                for (SQLSMALLINT i = 1; i <= ncols; ++i)
                {
                    SQLCHAR name[1024] = {};
                    SQLSMALLINT name_len = 0;
                    SQLSMALLINT sqltype = 0;
                    SQLULEN colsize = 0;
                    SQLSMALLINT dec = 0;
                    SQLSMALLINT nullable = 0;
                    rc = SQLDescribeCol(s.stmt,
                                        i,
                                        name,
                                        static_cast<SQLSMALLINT>(sizeof(name)),
                                        &name_len,
                                        &sqltype,
                                        &colsize,
                                        &dec,
                                        &nullable);
                    check_ok(rc, s.stmt, SQL_HANDLE_STMT, "odbc describe col");
                    // 列名超出固定缓冲：name_len 是完整长度（截断时驱动报 SQL_SUCCESS_WITH_INFO），
                    // 动态分配后重取，避免 std::string(name, name_len) 越界读或元数据丢失。
                    std::vector<SQLCHAR> big;
                    SQLCHAR* name_ptr = name;
                    if (name_len >= static_cast<SQLSMALLINT>(sizeof(name)))
                    {
                        big.resize(static_cast<size_t>(name_len) + 1);
                        name_ptr = big.data();
                        rc = SQLDescribeCol(s.stmt,
                                            i,
                                            name_ptr,
                                            static_cast<SQLSMALLINT>(big.size()),
                                            &name_len,
                                            &sqltype,
                                            &colsize,
                                            &dec,
                                            &nullable);
                        check_ok(rc, s.stmt, SQL_HANDLE_STMT, "odbc describe col (long name)");
                    }
                    rs.names.emplace_back(reinterpret_cast<char*>(name_ptr), static_cast<size_t>(name_len));
                    rs.types.push_back(map_sql_type(sqltype));
                    sqltypes.push_back(sqltype);
                }

                while (true)
                {
                    rc = co_await stmt_async(s, [&] { return SQLFetch(s.stmt); });
                    if (rc == SQL_NO_DATA)
                    {
                        break;
                    }
                    check_ok(rc, s.stmt, SQL_HANDLE_STMT, "odbc fetch");
                    std::vector<field> values;
                    values.reserve(static_cast<size_t>(ncols));
                    for (SQLSMALLINT i = 1; i <= ncols; ++i)
                    {
                        values.push_back(co_await read_field(s.stmt,
                                                             *s.obj,
                                                             i,
                                                             rs.types[static_cast<size_t>(i - 1)],
                                                             sqltypes[static_cast<size_t>(i - 1)]));
                    }
                    rs.rows.push_back(std::move(values));
                }
            }
            sets.push_back(std::move(rs));

            rc = co_await stmt_async(s, [&] { return SQLMoreResults(s.stmt); });
            if (rc == SQL_NO_DATA)
            {
                break;
            }
            check_ok(rc, s.stmt, SQL_HANDLE_STMT, "odbc more results");
        }
        co_return result(std::move(sets));
    }

    net::awaitable<statement_handle>
    odbc_backend::prepare(std::string_view sql)
    {
        if (!dbc_)
        {
            throw db_exception(boost::system::error_code {}, "db: odbc not connected");
        }
        auto s = new_stmt();
        std::string sql_str(sql);
        try
        {
            SQLRETURN rc = co_await stmt_async(*s,
                                               [&]
                                               {
                                                   return SQLPrepare(s->stmt,
                                                                     reinterpret_cast<SQLCHAR*>(sql_str.data()),
                                                                     static_cast<SQLINTEGER>(sql_str.size()));
                                               });
            check_ok(rc, s->stmt, SQL_HANDLE_STMT, "odbc prepare");
            co_return statement_handle { std::shared_ptr<odbc_stmt>(
                s.release(),
                [](odbc_stmt* p)
                {
                    odbc_backend::free_stmt(*p);
                    delete p;
                }) };
        }
        catch (...)
        {
            free_stmt(*s);
            throw;
        }
    }

    net::awaitable<result>
    odbc_backend::execute_statement(statement_handle h, std::vector<param> const& params)
    {
        auto* s = static_cast<odbc_stmt*>(h.state.get());
        if (!s || !s->stmt)
        {
            throw db_exception(boost::system::error_code {}, "db: odbc statement not prepared");
        }

        // 复用语句：关闭游标并清除上一轮绑定。
        SQLFreeStmt(s->stmt, SQL_CLOSE);
        SQLFreeStmt(s->stmt, SQL_RESET_PARAMS);

        std::vector<odbc_bind> binds;
        binds.reserve(params.size());
        for (auto const& p : params)
        {
            binds.push_back(make_bind(p));
        }
        for (size_t i = 0; i < binds.size(); ++i)
        {
            SQLRETURN rc = SQLBindParameter(s->stmt,
                                            static_cast<SQLUSMALLINT>(i + 1),
                                            SQL_PARAM_INPUT,
                                            binds[i].ctype,
                                            binds[i].sqltype,
                                            binds[i].colsize,
                                            binds[i].dec,
                                            binds[i].ptr(),
                                            binds[i].len,
                                            &binds[i].ind);
            check_ok(rc, s->stmt, SQL_HANDLE_STMT, "odbc bind parameter");
        }
        SQLRETURN rc = co_await stmt_async(*s, [&] { return SQLExecute(s->stmt); });
        check_ok(rc, s->stmt, SQL_HANDLE_STMT, "odbc execute statement");
        co_return co_await read_all(*s);
    }

    net::awaitable<void>
    odbc_backend::close_statement(statement_handle h) noexcept
    {
        h.state.reset(); // 自定义 deleter：free_stmt + delete
        co_return;
    }

    net::awaitable<void>
    odbc_backend::begin()
    {
        SQLRETURN rc = co_await conn_async(
            [&]
            {
                return SQLSetConnectAttr(static_cast<SQLHDBC>(dbc_),
                                         SQL_ATTR_AUTOCOMMIT,
                                         reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF),
                                         SQL_IS_UINTEGER);
            });
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc begin");
    }

    net::awaitable<void>
    odbc_backend::commit()
    {
        SQLRETURN rc
            = co_await conn_async([&] { return SQLEndTran(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc_), SQL_COMMIT); });
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc commit");
        rc = co_await conn_async(
            [&]
            {
                return SQLSetConnectAttr(static_cast<SQLHDBC>(dbc_),
                                         SQL_ATTR_AUTOCOMMIT,
                                         reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON),
                                         SQL_IS_UINTEGER);
            });
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc autocommit restore");
    }

    net::awaitable<void>
    odbc_backend::rollback()
    {
        SQLRETURN rc
            = co_await conn_async([&] { return SQLEndTran(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc_), SQL_ROLLBACK); });
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc rollback");
        rc = co_await conn_async(
            [&]
            {
                return SQLSetConnectAttr(static_cast<SQLHDBC>(dbc_),
                                         SQL_ATTR_AUTOCOMMIT,
                                         reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON),
                                         SQL_IS_UINTEGER);
            });
        check_ok(rc, dbc_, SQL_HANDLE_DBC, "odbc autocommit restore");
    }

    void
    register_odbc_backend()
    {
        register_backend("odbc",
                         [](net::any_io_executor ex, options const& opts) -> std::unique_ptr<backend>
                         {
                             odbc_config cfg;
                             cfg.connection_string = opts.get_or("connection_string", cfg.connection_string);
                             cfg.dsn = opts.get_or("dsn", cfg.dsn);
                             cfg.user = opts.get_or("uid", cfg.user);
                             cfg.password = opts.get_or("pwd", cfg.password);
                             return std::make_unique<odbc_backend>(std::move(ex), std::move(cfg));
                         });
    }

} // namespace httplib::db::detail
#endif // HTTPLIB_ENABLED_DATABASE