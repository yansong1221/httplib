#ifdef HTTPLIB_ENABLED_DATABASE
#include "session_impl.h"
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>
#include <cctype>

namespace httplib::mysql::detail
{

    static std::string
    quote_mysql_string(std::string_view sv)
    {
        std::string out;
        out.reserve(sv.size() + 2);
        out += '\'';
        for (char c : sv)
        {
            // 反斜杠与单引号都要翻倍：MySQL 默认 NO_BACKSLASH_ESCAPES 关闭时 \ 是转义符，
            // 不转义反斜杠会吞掉闭合引号或把 \n/\b 等解释成控制字符。
            if (c == '\'' || c == '\\')
            {
                out += c;
                out += c;
            }
            else
            {
                out += c;
            }
        }
        out += '\'';
        return out;
    }

    std::string
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
            return quote_mysql_string(f.as_string());
        }
        if (f.is_blob())
        {
            static const char* digits = "0123456789ABCDEF";
            auto b = f.as_blob();
            std::string out;
            out.reserve(b.size() * 2 + 3);
            out += "X'";
            for (unsigned char byte : b)
            {
                out += digits[(byte >> 4) & 0xF];
                out += digits[byte & 0xF];
            }
            out += '\'';
            return out;
        }
        if (f.is_date())
        {
            auto d = f.as_date();
            return std::to_string(static_cast<int>(d.year())) + "-" + std::to_string(static_cast<int>(d.month()))
                   + "-" + std::to_string(static_cast<int>(d.day()));
        }
        if (f.is_datetime())
        {
            auto d = f.as_datetime();
            return datetime { d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second(), d.microsecond() }
                .to_string();
        }
        if (f.is_time())
        {
            return time::from_duration(f.as_time()).to_string();
        }
        return "?";
    }

    static std::string
    param_to_sql(param const& p, std::chrono::seconds utc_offset)
    {
        return std::visit(
            [&](auto const& v) -> std::string
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>)
                {
                    return quote_mysql_string(v);
                }
                else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>)
                {
                    return quote_mysql_string(datetime::from_time_point(v + utc_offset).to_string());
                }
                else
                {
                    return format_param(v);
                }
            },
            p);
    }

    std::string
    render_query(std::string_view sql, std::vector<binder> const& binders, std::chrono::seconds utc_offset)
    {
        bool has_named = false;
        bool has_pos = false;
        std::unordered_map<std::string, param const*> named;
        std::vector<param const*> pos;
        for (auto const& b : binders)
        {
            if (b.name.empty())
            {
                has_pos = true;
                pos.push_back(&b.value);
            }
            else
            {
                has_named = true;
                named.emplace(b.name, &b.value);
            }
        }
        if (has_named && has_pos)
        {
            throw std::runtime_error("db: cannot mix positional and named parameters");
        }

        std::string out;
        out.reserve(sql.size());
        size_t pos_idx = 0;

        for (size_t i = 0; i < sql.size(); ++i)
        {
            char c = sql[i];
            if (c == '\'' || c == '"' || c == '`')
            {
                char quote = c;
                out += c;
                while (++i < sql.size())
                {
                    out += sql[i];
                    if (sql[i] == quote && (i + 1 >= sql.size() || sql[i + 1] != quote))
                    {
                        break;
                    }
                    if (sql[i] == '\\' && i + 1 < sql.size())
                    {
                        out += sql[++i];
                    }
                }
                continue;
            }
            if (c == '#')
            {
                while (i < sql.size() && sql[i] != '\n')
                {
                    ++i;
                }
                if (i < sql.size())
                {
                    out += sql[i];
                }
                continue;
            }
            if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-'
                && (i + 2 >= sql.size() || std::isspace(static_cast<unsigned char>(sql[i + 2]))))
            {
                while (i < sql.size() && sql[i] != '\n')
                {
                    ++i;
                }
                if (i < sql.size())
                {
                    out += sql[i];
                }
                continue;
            }
            if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/'))
                {
                    ++i;
                }
                i += 1;
                continue;
            }
            if (c == ':')
            {
                size_t start = i + 1;
                while (start < sql.size()
                       && (std::isalnum(static_cast<unsigned char>(sql[start])) || sql[start] == '_'))
                {
                    ++start;
                }
                if (start > i + 1)
                {
                    std::string name(sql.substr(i + 1, start - i - 1));
                    param const* pv = nullptr;
                    if (has_named)
                    {
                        auto it = named.find(name);
                        if (it == named.end())
                        {
                            throw std::runtime_error("db: unbound named parameter ':" + name + "'");
                        }
                        pv = it->second;
                    }
                    else
                    {
                        if (pos_idx >= pos.size())
                        {
                            throw std::runtime_error("db: too few parameters for placeholders");
                        }
                        pv = pos[pos_idx++];
                    }
                    out += param_to_sql(*pv, utc_offset);
                    i = start - 1;
                    continue;
                }
            }
            out += c;
        }

        if (!has_named && pos_idx != pos.size())
        {
            throw std::runtime_error("db: too many parameters");
        }
        return out;
    }

    static void
    raise_mysql_error(boost::system::error_code ec,
                      boost::mysql::diagnostics const& diag,
                      std::string_view sql,
                      std::string_view params)
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

    static void
    raise_mysql_error(boost::system::error_code ec, boost::mysql::diagnostics const& diag)
    {
        raise_mysql_error(ec, diag, {}, {});
    }

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

    net::awaitable<std::chrono::seconds>
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

        conn.set_meta_mode(boost::mysql::metadata_mode::full);

        if (!cfg.charset.empty())
        {
            boost::mysql::results r;
            boost::mysql::diagnostics diag;
            boost::system::error_code ec;
            co_await conn.async_execute("SET NAMES " + quote_mysql_string(cfg.charset),
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
            co_await conn.async_execute("SET time_zone = " + quote_mysql_string(cfg.time_zone),
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

} // namespace httplib::mysql::detail

namespace httplib::mysql
{

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
        detail::raise_mysql_error(ec, diag, sql, params);
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
