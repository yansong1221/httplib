#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

namespace httplib::mysql
{
    /// 日期（年/月/日）。
    struct date;
    /// 日期时间（date + time）。
    struct datetime;
    /// 时间（时/分/秒/微秒）。
    struct time;

    class result;
    class session;
    class connection_pool;
    class row;
    class prepared_statement;

    struct query_log_entry;

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
