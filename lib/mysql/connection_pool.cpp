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

    struct connection_pool::impl
    {
        pool_params c;
        std::unique_ptr<boost::mysql::connection_pool> pool;
        std::atomic<bool> stopped { true };
    };

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

    connection_pool::connection_pool(net::any_io_executor ex, pool_params c) : impl_(std::make_unique<impl>())
    {
        impl_->c = std::move(c);
        impl_->pool = std::make_unique<boost::mysql::connection_pool>(ex, make_pool_params(impl_->c));
    }

    connection_pool::~connection_pool() { stop(); }

    connection_pool::connection_pool(connection_pool&&) noexcept = default;
    connection_pool& connection_pool::operator=(connection_pool&&) noexcept = default;

    void
    connection_pool::start()
    {
        if (!impl_->stopped.exchange(false))
        {
            return;
        }
        impl_->pool->async_run(net::detached);
    }

    net::awaitable<session>
    connection_pool::async_acquire(std::chrono::steady_clock::duration wait_timeout)
    {
        if (impl_->stopped)
        {
            throw std::runtime_error("connection_pool: not started");
        }
        auto pooled = co_await (wait_timeout <= std::chrono::steady_clock::duration::zero()
                                    ? impl_->pool->async_get_connection(boost::asio::use_awaitable)
                                    : impl_->pool->async_get_connection(
                                          boost::asio::cancel_after(wait_timeout, boost::asio::use_awaitable)));

        auto sess_impl = std::make_unique<session::impl>();
        sess_impl->pooled = std::move(pooled);
        co_return session(std::move(sess_impl));
    }

    void
    connection_pool::stop()
    {
        if (impl_->stopped.exchange(true))
        {
            return;
        }
        impl_->pool->cancel();
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
