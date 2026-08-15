#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/session.hpp"
#include "httplib/mysql/connection_pool.hpp"
#include "mysql/result_impl.h"
#include "mysql/session_impl.h"
#include "mysql/util.hpp"
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>
#include <cctype>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace httplib::mysql
{
    namespace detail
    {
        /// 建立连接并完成会话初始化（charset / time_zone / 元数据模式），返回相对 UTC 的偏移（秒）。
        static net::awaitable<std::chrono::seconds>
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
                co_await conn.async_execute("SET NAMES " + util::quote_mysql_string(cfg.charset),
                                            r,
                                            diag,
                                            net::redirect_error(net::use_awaitable, ec));
                util::raise_mysql_error(ec, diag);
            }

            if (!cfg.time_zone.empty())
            {
                boost::mysql::results r;
                boost::mysql::diagnostics diag;
                boost::system::error_code ec;
                co_await conn.async_execute("SET time_zone = " + util::quote_mysql_string(cfg.time_zone),
                                            r,
                                            diag,
                                            net::redirect_error(net::use_awaitable, ec));
                util::raise_mysql_error(ec, diag);
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
            util::raise_mysql_error(ec, diag);
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

        // 公开的标量参数 → Boost.MySQL 字段视图（仅内部使用）
        static boost::mysql::field_view
        to_field_view(std::monostate)
        {
            return {};
        }
        static boost::mysql::field_view
        to_field_view(int64_t v)
        {
            return boost::mysql::field_view(v);
        }
        static boost::mysql::field_view
        to_field_view(uint64_t v)
        {
            return boost::mysql::field_view(v);
        }
        static boost::mysql::field_view
        to_field_view(double v)
        {
            return boost::mysql::field_view(v);
        }
        static boost::mysql::field_view
        to_field_view(std::span<std::byte const> v)
        {
            return boost::mysql::field_view(
                boost::mysql::blob_view(reinterpret_cast<unsigned char const*>(v.data()), v.size()));
        }
        static boost::mysql::field_view
        to_field_view(date v)
        {
            return boost::mysql::field_view(boost::mysql::date(v.year, v.month, v.day));
        }
        static boost::mysql::field_view
        to_field_view(datetime v)
        {
            return boost::mysql::field_view(
                boost::mysql::datetime(v.year, v.month, v.day, v.hour, v.minute, v.second, v.microsecond));
        }
        static boost::mysql::field_view
        to_field_view(time v)
        {
            return boost::mysql::field_view(v.to_duration());
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
                        return util::quote_mysql_string(v);
                    }
                    else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>)
                    {
                        return util::quote_mysql_string(datetime::from_time_point(v + utc_offset).to_string());
                    }
                    else
                    {
                        return util::format_param(to_field_view(v));
                    }
                },
                p);
        }

        static std::string
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

    } // namespace detail

    session::impl::impl()
    {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        spdlog::sinks_init_list sink_list = { console_sink };
        default_logger_ = std::make_shared<spdlog::logger>("httplib.mysql", sink_list);
        default_logger_->set_level(spdlog::level::info);
    }

    session::session(std::unique_ptr<impl> p) : impl_(std::move(p)) {}

    net::awaitable<session>
    session::connect(net::any_io_executor ex, connect_params cfg)
    {
        auto conn = std::make_unique<boost::mysql::any_connection>(ex);
        auto offset = co_await detail::connect_session(*conn, cfg);

        auto imp = std::make_unique<impl>();
        imp->params = std::move(cfg);
        imp->conn = std::move(conn);
        imp->utc_offset = offset;
        imp->stmt_cache.capacity = imp->params.max_cached_statements;
        co_return session(std::move(imp));
    }

    session::session(session&&) noexcept = default;
    session& session::operator=(session&&) noexcept = default;

    session::~session() = default;

    session::impl&
    get_impl(session& self)
    {
        return *self.impl_;
    }

    session::impl const&
    get_impl(session const& self)
    {
        return *self.impl_;
    }

    net::awaitable<result>
    session::query(std::string_view sql)
    {
        auto& imp = get_impl(*this);

        auto start = std::chrono::steady_clock::now();

        boost::mysql::diagnostics diag;
        boost::system::error_code ec;
        boost::mysql::results data;
        co_await imp.get_conn().async_execute(sql,
                                              data,
                                              diag,
                                              boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        imp.raise_error(ec, diag, sql);
        imp.touch();

        auto res = result(std::make_unique<result::impl>(std::move(data), imp.utc_offset));

        if (imp.query_logger)
        {
            query_log_entry entry;
            entry.sql = std::string(sql);
            entry.duration = std::chrono::steady_clock::now() - start;
            entry.row_count = res.row_count();
            entry.affected_rows = res.affected_rows();
            imp.query_logger(entry);
        }

        co_return res;
    }

    net::awaitable<result>
    session::execute_query(std::string_view sql, std::vector<detail::binder> binders)
    {
        if (binders.empty())
        {
            co_return co_await query(sql);
        }
        auto& imp = get_impl(*this);
        auto rendered = detail::render_query(sql, binders, imp.utc_offset);
        co_return co_await query(rendered);
    }

    prepared_statement
    session::stmt(std::string_view sql)
    {
        return prepared_statement(*this, std::string(sql));
    }

    net::awaitable<void>
    session::begin_transaction()
    {
        co_await get_impl(*this).begin_transaction();
    }

    net::awaitable<void>
    session::reconnect()
    {
        auto& imp = get_impl(*this);

        imp.conn->close();
        imp.clear_statement_cache();
        imp.conn = std::make_unique<boost::mysql::any_connection>(imp.conn->get_executor());

        imp.utc_offset = co_await detail::connect_session(*imp.conn, imp.params);
        imp.live = true;
        imp.in_transaction = false;
    }

    net::awaitable<bool>
    session::ping()
    {
        auto& imp = get_impl(*this);
        try
        {
            co_await imp.get_conn().async_ping(boost::asio::use_awaitable);
            imp.last_ping = std::chrono::steady_clock::now();
            co_return true;
        }
        catch (...)
        {
            imp.live = false;
            co_return false;
        }
    }

    void
    session::set_query_logger(query_logger cb)
    {
        get_impl(*this).query_logger = std::move(cb);
    }

    std::shared_ptr<spdlog::logger>
    session::logger() const
    {
        return get_impl(*this).logger();
    }

    void
    session::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        get_impl(*this).set_logger(std::move(logger));
    }

    std::chrono::steady_clock::time_point
    session::last_active_time() const
    {
        return get_impl(*this).last_active;
    }

    std::chrono::steady_clock::time_point
    session::last_ping_time() const
    {
        return get_impl(*this).last_ping;
    }

    bool
    session::in_transaction() const
    {
        return get_impl(*this).in_transaction;
    }

    net::awaitable<void>
    session::commit()
    {
        co_await get_impl(*this).commit();
    }

    net::awaitable<void>
    session::rollback()
    {
        co_await get_impl(*this).rollback();
    }

    net::awaitable<void>
    session::impl::begin_transaction()
    {
        boost::mysql::results r;
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;
        co_await get_conn().async_execute("START TRANSACTION",
                                          r,
                                          diag,
                                          boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        raise_error(ec, diag);
        in_transaction = true;
    }

    net::awaitable<void>
    session::impl::commit()
    {
        boost::mysql::results r;
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;
        co_await get_conn().async_execute("COMMIT",
                                          r,
                                          diag,
                                          boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        raise_error(ec, diag);
        in_transaction = false;
    }

    net::awaitable<void>
    session::impl::rollback()
    {
        boost::mysql::results r;
        boost::mysql::diagnostics diag;
        boost::system::error_code ec;
        co_await get_conn().async_execute("ROLLBACK",
                                          r,
                                          diag,
                                          boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        raise_error(ec, diag);
        in_transaction = false;
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
