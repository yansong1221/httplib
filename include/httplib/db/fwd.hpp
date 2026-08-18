#pragma once
#include <cstdint>

namespace httplib::db
{
    /// 日期（年/月/日）。
    struct date;
    /// 日期时间（date + time）。
    struct datetime;
    /// 时间（时/分/秒/微秒）。
    struct time;

    class result;
    class row;
    class session;
    class prepared_statement;
    class connection_pool;
    class db_exception;

    struct query_log_entry;
} // namespace httplib::db
