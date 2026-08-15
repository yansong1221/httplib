#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/session.hpp"
#include "httplib/mysql/connection_pool.hpp"
#include "mysql/result_impl.h"
#include "mysql/session_impl.h"
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace httplib::mysql
{

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
