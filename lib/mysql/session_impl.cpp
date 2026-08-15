#ifdef HTTPLIB_ENABLED_DATABASE
#include "session_impl.h"
#include "util.hpp"
#include <boost/mysql.hpp>

namespace httplib::mysql
{

    namespace detail
    {
        static bool
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
    } // namespace detail

    std::shared_ptr<spdlog::logger>
    session::impl::logger() const
    {
        return custom_logger_ ? custom_logger_ : default_logger_;
    }

    void
    session::impl::set_logger(std::shared_ptr<spdlog::logger> l)
    {
        custom_logger_ = std::move(l);
    }

    void
    session::impl::raise_error(boost::system::error_code ec,
                               boost::mysql::diagnostics const& diag,
                               std::string_view sql,
                               std::string_view params)
    {
        if (!ec)
        {
            return;
        }
        if (detail::is_connection_lost(ec, diag))
        {
            live = false;
        }
        util::raise_mysql_error(ec, diag, sql, params);
    }

    boost::mysql::statement*
    session::impl::find_statement(std::string_view sql)
    {
        auto it = stmt_cache.map.find(sql);
        if (it == stmt_cache.map.end())
        {
            return nullptr;
        }
        stmt_cache.lru.splice(stmt_cache.lru.begin(), stmt_cache.lru, it->second.lru_it);
        return &it->second.stmt;
    }

    std::optional<boost::mysql::statement>
    session::impl::store_statement(std::string sql, boost::mysql::statement stmt)
    {
        if (stmt_cache.capacity == 0)
        {
            return std::move(stmt);
        }
        std::optional<boost::mysql::statement> evicted;
        if (stmt_cache.map.size() >= stmt_cache.capacity)
        {
            auto evict_key = std::move(stmt_cache.lru.back());
            stmt_cache.lru.pop_back();
            auto evict_it = stmt_cache.map.find(evict_key);
            if (evict_it != stmt_cache.map.end())
            {
                evicted = std::move(evict_it->second.stmt);
                stmt_cache.map.erase(evict_it);
            }
        }
        stmt_cache.lru.push_front(sql);
        stmt_cache.map.emplace(std::move(sql), statement_cache::entry { std::move(stmt), stmt_cache.lru.begin() });
        return evicted;
    }

    void
    session::impl::clear_statement_cache()
    {
        stmt_cache.map.clear();
        stmt_cache.lru.clear();
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
