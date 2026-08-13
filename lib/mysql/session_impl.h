#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/mysql/mysql_exception.hpp"
#include "httplib/mysql/prepared_statement.hpp"
#include "httplib/mysql/session.hpp"
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>
#include <boost/mysql/diagnostics.hpp>
#include <boost/mysql/field_view.hpp>
#include <boost/mysql/statement.hpp>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace httplib::mysql
{

    inline std::string
    format_param(boost::mysql::field_view const& f)
    {
        if (f.is_null())
        {
            return "NULL";
        }
        if (f.is_int64())
        {
            return std::to_string(f.as_int64());
        }
        if (f.is_uint64())
        {
            return std::to_string(f.as_uint64());
        }
        if (f.is_double())
        {
            return std::to_string(f.as_double());
        }
        if (f.is_string())
        {
            std::string out = "'";
            for (char c : f.as_string())
            {
                if (c == '\'')
                {
                    out += "''";
                }
                else
                {
                    out += c;
                }
            }
            out += "'";
            return out;
        }
        if (f.is_blob())
        {
            return "(blob " + std::to_string(f.as_blob().size()) + " bytes)";
        }
        if (f.is_date())
        {
            auto d = f.as_date();
            return std::to_string(static_cast<int>(d.year())) + "-" + std::to_string(static_cast<int>(d.month())) + "-"
                   + std::to_string(static_cast<int>(d.day()));
        }
        if (f.is_datetime())
        {
            auto d = f.as_datetime();
            return std::to_string(static_cast<int>(d.year())) + "-" + std::to_string(static_cast<int>(d.month())) + "-"
                   + std::to_string(static_cast<int>(d.day())) + " " + std::to_string(static_cast<int>(d.hour())) + ":"
                   + std::to_string(static_cast<int>(d.minute())) + ":" + std::to_string(static_cast<int>(d.second()));
        }
        return "?";
    }

    inline std::string
    format_params(std::vector<boost::mysql::field_view> const& params)
    {
        if (params.empty())
        {
            return {};
        }
        std::string out = "[";
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (i != 0)
            {
                out += ", ";
            }
            out += format_param(params[i]);
        }
        out += "]";
        return out;
    }

    inline std::string
    format_named_params(std::vector<std::string> const& names, std::vector<boost::mysql::field_view> const& params)
    {
        if (params.empty())
        {
            return {};
        }
        std::string out = "[";
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (i != 0)
            {
                out += ", ";
            }
            if (i < names.size())
            {
                out += names[i];
                out += "=";
            }
            out += format_param(params[i]);
        }
        out += "]";
        return out;
    }

    inline void
    raise_mysql_error(boost::system::error_code ec,
                      boost::mysql::diagnostics const& diag,
                      std::string_view sql,
                      std::string_view params = {})
    {
        if (!ec)
        {
            return;
        }
        auto what
            = std::string("[") + std::to_string(ec.value()) + "] " + ec.message() + " (SQL: " + std::string(sql) + ")";
        if (!params.empty())
        {
            what += " params: " + std::string(params);
        }
        auto msg = diag.server_message();
        if (!msg.empty())
        {
            what += ": " + std::string(msg.data(), msg.size());
        }
        throw mysql_exception(ec, what);
    }

    inline void
    raise_mysql_error(boost::system::error_code ec, boost::mysql::diagnostics const& diag)
    {
        raise_mysql_error(ec, diag, {});
    }

    inline bool
    is_connection_lost(boost::system::error_code ec, boost::mysql::diagnostics const& diag)
    {
        if (!ec)
        {
            return false;
        }
        // 服务端 SQL 错误（语法、约束等）连接仍可用
        if (!diag.server_message().empty())
        {
            return false;
        }
        // 参数数量不匹配是客户端在发送前就判定的，连接仍可用
        if (ec == boost::mysql::client_errc::wrong_num_params)
        {
            return false;
        }
        return true;
    }

    inline net::awaitable<std::chrono::seconds>
    connect_session(boost::mysql::any_connection& conn, connect_params const& cfg)
    {
        boost::mysql::connect_params params;
        params.server_address.emplace_host_and_port(cfg.host, cfg.port);
        params.username = cfg.user;
        params.password = cfg.password;
        if (!cfg.database.empty())
        {
            params.database = cfg.database;
        }
        params.ssl = cfg.ssl ? boost::mysql::ssl_mode::enable : boost::mysql::ssl_mode::disable;
        params.multi_queries = true;

        if (cfg.connect_timeout.count() > 0)
        {
            co_await conn.async_connect(params, net::cancel_after(cfg.connect_timeout, net::use_awaitable));
        }
        else
        {
            co_await conn.async_connect(params, net::use_awaitable);
        }

        if (!cfg.charset.empty())
        {
            boost::mysql::results r;
            boost::mysql::diagnostics diag;
            boost::system::error_code ec;
            co_await conn.async_execute("SET NAMES '" + cfg.charset + "'",
                                        r,
                                        diag,
                                        net::redirect_error(net::use_awaitable, ec));
            raise_mysql_error(ec, diag);
        }

        if (!cfg.time_zone.empty())
        {
            boost::mysql::results r;
            boost::mysql::diagnostics diag;
            boost::system::error_code ec;
            co_await conn.async_execute("SET time_zone = '" + cfg.time_zone + "'",
                                        r,
                                        diag,
                                        net::redirect_error(net::use_awaitable, ec));
            raise_mysql_error(ec, diag);
        }

        // 会话相对 UTC 的偏移（秒），用于 TIMESTAMP 类型换算回 UTC
        std::chrono::seconds offset { 0 };
        boost::mysql::results r;
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;
        co_await conn.async_execute("SELECT TIMESTAMPDIFF(SECOND, UTC_TIMESTAMP(), NOW())",
                                    r,
                                    diag,
                                    net::redirect_error(net::use_awaitable, ec));
        raise_mysql_error(ec, diag);
        if (r.has_value() && !r.rows().empty())
        {
            auto f = r.rows()[0][0];
            if (f.is_int64())
            {
                offset = std::chrono::seconds(f.as_int64());
            }
            else if (f.is_uint64())
            {
                offset = std::chrono::seconds(static_cast<int64_t>(f.as_uint64()));
            }
        }
        co_return offset;
    }

    struct session::impl
    {
        std::unique_ptr<boost::mysql::any_connection> conn;
        connect_params params;
        bool in_transaction = false;
        bool live = true;
        session::query_logger query_logger;

        std::chrono::steady_clock::time_point last_active = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_ping;
        std::chrono::seconds utc_offset { 0 };

        std::vector<boost::mysql::statement> stmts_to_close;

        struct statement_cache
        {
            struct entry
            {
                boost::mysql::statement stmt;
                std::list<std::string>::iterator lru_it;
            };

            std::unordered_map<std::string, entry> map;
            std::list<std::string> lru;
            size_t capacity = 64;
        } stmt_cache;

        boost::mysql::any_connection&
        get_conn()
        {
            return *conn;
        }

        void
        raise_error(boost::system::error_code ec,
                    boost::mysql::diagnostics const& diag,
                    std::string_view sql = {},
                    std::string_view params = {})
        {
            if (!ec)
            {
                return;
            }
            if (is_connection_lost(ec, diag))
            {
                live = false;
            }
            raise_mysql_error(ec, diag, sql, params);
        }

        boost::mysql::statement*
        find_statement(std::string_view sql)
        {
            auto it = stmt_cache.map.find(std::string(sql));
            if (it == stmt_cache.map.end())
            {
                return nullptr;
            }
            stmt_cache.lru.splice(stmt_cache.lru.begin(), stmt_cache.lru, it->second.lru_it);
            return &it->second.stmt;
        }

        void
        store_statement(std::string sql, boost::mysql::statement stmt)
        {
            if (stmt_cache.capacity == 0)
            {
                stmts_to_close.push_back(std::move(stmt));
                return;
            }
            if (stmt_cache.map.size() >= stmt_cache.capacity)
            {
                auto evict_key = std::move(stmt_cache.lru.back());
                stmt_cache.lru.pop_back();
                auto evict_it = stmt_cache.map.find(evict_key);
                if (evict_it != stmt_cache.map.end())
                {
                    stmts_to_close.push_back(std::move(evict_it->second.stmt));
                    stmt_cache.map.erase(evict_it);
                }
            }
            stmt_cache.lru.push_front(sql);
            stmt_cache.map.emplace(std::move(sql), statement_cache::entry { std::move(stmt), stmt_cache.lru.begin() });
        }

        void
        clear_statement_cache()
        {
            stmt_cache.map.clear();
            stmt_cache.lru.clear();
        }

        net::awaitable<void> begin_transaction();
        net::awaitable<void> commit();
        net::awaitable<void> rollback();
    };

    struct prepared_statement::impl
    {
        session* session = nullptr;
        std::string sql;
        std::string original_sql;
        std::vector<boost::mysql::field_view> params;

        std::vector<std::function<void(result const&)>> extractors;

        std::vector<std::string> param_names;
        std::unordered_map<std::string, size_t> name_to_idx;
        std::unordered_map<std::string, boost::mysql::field_view> named_values;
        std::unordered_map<std::string, std::string> named_storage;
        std::deque<std::string> data_strs;
        bool parsed = false;
        bool need_params_reset = false;
        bool need_extractors_reset = false;
        bool used_positional_bind = false;

        void
        begin_bind()
        {
            used_positional_bind = true;
            if (need_params_reset)
            {
                params.clear();
                data_strs.clear();
                need_params_reset = false;
            }
        }

        void
        add_extractor(std::function<void(result const&)> ex)
        {
            if (need_extractors_reset)
            {
                extractors.clear();
                need_extractors_reset = false;
            }
            extractors.push_back(std::move(ex));
        }
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
