#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#    include "httplib/db/db_session.hpp"
#    include "httplib/db/prepared_statement.hpp"
#    include "httplib/db/transaction.hpp"
#    include <boost/mysql/connection_pool.hpp>
#    include <boost/mysql/field_view.hpp>
#    include <boost/mysql/statement.hpp>
#    include <functional>
#    include <string>
#    include <unordered_map>
#    include <vector>

namespace httplib::db
{

    struct db_session::impl
    {
        boost::mysql::pooled_connection pooled;
        bool in_transaction = false;
        db_session::query_logger query_logger;

        net::awaitable<void> begin_transaction();
        net::awaitable<void> commit();
        net::awaitable<void> rollback();

        net::awaitable<db_result> query_raw(std::string_view sql, std::span<boost::mysql::field_view const> params);
    };

    struct prepared_statement::impl
    {
        db_session* session = nullptr;
        std::string sql;
        std::vector<boost::mysql::field_view> params;

        boost::mysql::statement stmt;
        bool stmt_prepared = false;

        std::vector<std::function<void(db_result const&)>> extractors;

        std::vector<std::string> param_names;
        std::unordered_map<std::string, size_t> name_to_idx;
        bool parsed = false;
    };

    struct transaction::impl
    {
        db_session* session = nullptr;
        bool committed = false;
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
