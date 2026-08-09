#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/db/db_pool.hpp"
#include "db/db_pool_impl.h"
#include "db/db_session_impl.h"
#include "httplib/db/db_session.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>

namespace httplib::db
{

    static boost::mysql::pool_params
    make_pool_params(db_config const& config)
    {
        boost::mysql::pool_params params;
        params.server_address.emplace_host_and_port(config.host, config.port);
        params.username = config.user;
        params.password = config.password;
        params.database = config.database;
        params.initial_size = config.min_connections;
        params.max_size = config.max_connections;
        params.connect_timeout = config.connect_timeout;
        params.ping_interval = config.ping_interval;
        params.ping_timeout = config.ping_timeout;
        return params;
    }

    db_pool::db_pool(net::any_io_executor ex, db_config config) : impl_(std::make_unique<impl>())
    {
        impl_->config = std::move(config);
        impl_->pool = std::make_shared<boost::mysql::connection_pool>(ex, make_pool_params(impl_->config));
    }

    db_pool::~db_pool() = default;

    db_pool::db_pool(db_pool&&) noexcept = default;
    db_pool& db_pool::operator=(db_pool&&) noexcept = default;

    void
    db_pool::start()
    {
        impl_->pool->async_run(net::detached);
    }

    net::awaitable<db_session>
    db_pool::async_acquire(std::chrono::steady_clock::duration wait_timeout)
    {
        auto pooled = co_await (wait_timeout <= std::chrono::steady_clock::duration::zero()
                                    ? impl_->pool->async_get_connection(boost::asio::use_awaitable)
                                    : impl_->pool->async_get_connection(
                                          boost::asio::cancel_after(wait_timeout, boost::asio::use_awaitable)));

        auto sess_impl = std::make_unique<db_session::impl>();
        sess_impl->pooled = std::move(pooled);
        co_return db_session(std::move(sess_impl));
    }

    void
    db_pool::stop()
    {
        impl_->pool->cancel();
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
