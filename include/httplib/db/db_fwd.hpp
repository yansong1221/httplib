#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

namespace httplib::db
{
    struct db_date;
    struct db_datetime;

    class db_result;
    class db_session;
    class db_pool;
    class row;
    class prepared_statement;
    class transaction;

    struct query_log_entry;

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
