#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

namespace httplib::mysql
{
    struct date;
    struct datetime;
    struct time;

    class result;
    class session;
    class connection_pool;
    class row;
    class prepared_statement;

    struct query_log_entry;

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
