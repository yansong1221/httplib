#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/mysql/mysql_exception.hpp"
#include "httplib/mysql/prepared_statement.hpp"
#include "httplib/mysql/session.hpp"
#include <boost/mysql/connection_pool.hpp>
#include <boost/mysql/diagnostics.hpp>
#include <boost/mysql/field_view.hpp>
#include <boost/mysql/statement.hpp>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib::mysql
{

    inline void
    raise_mysql_error(boost::system::error_code ec,
                      boost::mysql::diagnostics const& diag,
                      std::string_view sql)
    {
        if (!ec)
            return;
        auto what = std::string("[") + std::to_string(ec.value()) + "] " + ec.message() + " (SQL: "
                    + std::string(sql) + ")";
        auto msg = diag.server_message();
        if (!msg.empty())
            what += ": " + std::string(msg.data(), msg.size());
        throw mysql_exception(ec, what);
    }

    inline void
    raise_mysql_error(boost::system::error_code ec, boost::mysql::diagnostics const& diag)
    {
        raise_mysql_error(ec, diag, {});
    }

    struct session::impl
    {
        boost::mysql::pooled_connection pooled;
        std::unique_ptr<boost::mysql::any_connection> standalone;
        connect_params params;
        bool in_transaction = false;
        session::query_logger query_logger;

        boost::mysql::any_connection&
        get_conn()
        {
            if (standalone)
            {
                return *standalone;
            }
            return pooled.get();
        }

        net::awaitable<void> begin_transaction();
        net::awaitable<void> commit();
        net::awaitable<void> rollback();
    };

    struct prepared_statement::impl
    {
        session* session = nullptr;
        std::string sql;
        std::vector<boost::mysql::field_view> params;

        boost::mysql::statement stmt;
        bool stmt_prepared = false;

        std::vector<std::function<void(result const&)>> extractors;

        std::vector<std::string> param_names;
        std::unordered_map<std::string, size_t> name_to_idx;
        std::string data_str;
        bool parsed = false;
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
