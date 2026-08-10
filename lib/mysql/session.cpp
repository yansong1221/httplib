#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/session.hpp"
#include "httplib/mysql/connection_pool.hpp"
#include "mysql/result_impl.h"
#include "mysql/session_impl.h"
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>
#include <stdexcept>

namespace httplib::mysql
{

    session::session(std::unique_ptr<impl> p) : impl_(std::move(p)) {}

    net::awaitable<session>
    session::connect(net::any_io_executor ex, connect_params cfg)
    {
        boost::mysql::any_connection conn(ex);
        boost::mysql::connect_params params;
        params.server_address.emplace_host_and_port(cfg.host, cfg.port);
        params.username = cfg.user;
        params.password = cfg.password;
        if (!cfg.database.empty())
        {
            params.database = cfg.database;
        }
        co_await conn.async_connect(params, boost::asio::use_awaitable);

        auto imp = std::make_unique<impl>();
        imp->params = std::move(cfg);
        imp->standalone = std::make_unique<boost::mysql::any_connection>(std::move(conn));
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
        co_return co_await stmt(sql).execute();
    }

    prepared_statement
    session::stmt(std::string_view sql)
    {
        return prepared_statement(*this, std::string(sql));
    }

    net::awaitable<void>
    session::reconnect()
    {
        auto& imp = get_impl(*this);
        if (!imp.standalone)
        {
            throw std::runtime_error("reconnect only available on standalone sessions");
        }

        imp.standalone->close();
        imp.standalone = std::make_unique<boost::mysql::any_connection>(imp.standalone->get_executor());

        boost::mysql::connect_params params;
        params.server_address.emplace_host_and_port(imp.params.host, imp.params.port);
        params.username = imp.params.user;
        params.password = imp.params.password;
        if (!imp.params.database.empty())
        {
            params.database = imp.params.database;
        }
        co_await imp.standalone->async_connect(params, boost::asio::use_awaitable);
    }

    net::awaitable<bool>
    session::ping()
    {
        try
        {
            co_await get_impl(*this).get_conn().async_ping(boost::asio::use_awaitable);
            co_return true;
        }
        catch (...)
        {
            co_return false;
        }
    }

    void
    session::set_query_logger(query_logger cb)
    {
        get_impl(*this).query_logger = std::move(cb);
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
        raise_mysql_error(ec, diag);
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
        raise_mysql_error(ec, diag);
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
        raise_mysql_error(ec, diag);
        in_transaction = false;
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
