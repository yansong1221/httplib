#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/connection_pool.hpp"
#include "httplib/mysql/session.hpp"
#include "mysql/session_impl.h"
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>

namespace httplib::mysql
{
    static boost::mysql::pool_params
    make_pool_params(pool_params const& cfg)
    {
        boost::mysql::pool_params params;
        params.server_address.emplace_host_and_port(cfg.host, cfg.port);
        params.username = cfg.user;
        params.password = cfg.password;
        params.database = cfg.database;
        params.initial_size = cfg.min_connections;
        params.max_size = cfg.max_connections;
        params.connect_timeout = cfg.connect_timeout;
        params.ping_interval = cfg.ping_interval;
        params.ping_timeout = cfg.ping_timeout;
        params.multi_queries = true;
        return params;
    }

    struct connection_pool::impl : public std::enable_shared_from_this<connection_pool::impl>
    {
        boost::mysql::connection_pool pool;
        std::atomic<bool> stopped { true };

        impl(net::any_io_executor const& ex, pool_params const& c) : pool(ex, make_pool_params(c)) {}
        void
        start()
        {
            if (!stopped.exchange(false))
            {
                return;
            }
            pool.async_run(net::detached);
        }
        void
        stop()
        {
            if (stopped.exchange(true))
            {
                return;
            }
            pool.cancel();
        }
        net::awaitable<session>
        async_acquire(std::chrono::steady_clock::duration wait_timeout)
        {
            if (stopped)
            {
                throw std::runtime_error("connection_pool: not started");
            }
            auto pooled = co_await (
                wait_timeout <= std::chrono::steady_clock::duration::zero()
                    ? pool.async_get_connection(boost::asio::use_awaitable)
                    : pool.async_get_connection(boost::asio::cancel_after(wait_timeout, boost::asio::use_awaitable)));

            auto sess_impl = std::make_unique<session::impl>();
            sess_impl->pooled = std::move(pooled);
            co_return session(std::move(sess_impl));
        }
    };

    connection_pool::connection_pool(net::any_io_executor ex, pool_params c) : impl_(std::make_shared<impl>(ex, c)) {}

    connection_pool::~connection_pool() {}

    connection_pool::connection_pool(connection_pool&&) noexcept = default;
    connection_pool& connection_pool::operator=(connection_pool&&) noexcept = default;

    void
    connection_pool::start()
    {
        impl_->start();
    }

    net::awaitable<session>
    connection_pool::async_acquire(std::chrono::steady_clock::duration wait_timeout)
    {
        co_return co_await impl_->async_acquire(wait_timeout);
    }

    void
    connection_pool::stop()
    {
        impl_->stop();
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
