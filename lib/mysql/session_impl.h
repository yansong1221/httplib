#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/mysql/binder.hpp"
#include "httplib/mysql/session.hpp"
#include "httplib/util/string_hash.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/mysql.hpp>
#include <boost/mysql/diagnostics.hpp>
#include <boost/mysql/statement.hpp>
#include <chrono>
#include <cstddef>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace httplib::mysql
{

    struct session::impl
    {
        std::unique_ptr<boost::mysql::any_connection> conn;
        connect_params params;
        bool in_transaction = false;
        bool live = true;
        session::query_logger query_logger;

        std::shared_ptr<spdlog::logger> default_logger_;
        std::shared_ptr<spdlog::logger> custom_logger_;

        std::chrono::steady_clock::time_point last_active = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_ping;
        std::chrono::seconds utc_offset { 0 };

        struct statement_cache
        {
            struct entry
            {
                boost::mysql::statement stmt;
                std::list<std::string>::iterator lru_it;
            };

            util::string_map<entry> map;
            std::list<std::string> lru;
            size_t capacity = 64;
        } stmt_cache;

        impl();

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> l);

        boost::mysql::any_connection&
        get_conn()
        {
            return *conn;
        }

        void
        touch()
        {
            last_active = std::chrono::steady_clock::now();
        }

        void raise_error(boost::system::error_code ec,
                         boost::mysql::diagnostics const& diag,
                         std::string_view sql = {},
                         std::string_view params = {});

        boost::mysql::statement* find_statement(std::string_view sql);

        // 返回需要关闭的语句：capacity==0 时是本次 prepare 的语句（用完即关），
        // 缓存满驱逐时是被逐出的旧语句；正常缓存则返回 nullopt。
        std::optional<boost::mysql::statement> store_statement(std::string sql, boost::mysql::statement stmt);

        void clear_statement_cache();

        net::awaitable<void> begin_transaction();
        net::awaitable<void> commit();
        net::awaitable<void> rollback();
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
