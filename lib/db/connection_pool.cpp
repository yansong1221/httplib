#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/db/connection_pool.hpp"
#include "connection_pool_impl.h"

namespace httplib::db
{

    // ---- session_handle ----

    connection_pool::session_handle::session_handle() {}

    connection_pool::session_handle::session_handle(std::weak_ptr<impl> pool, std::unique_ptr<session> sess)
        : pool_(std::move(pool))
        , sess_(std::move(sess))
    {
    }

    connection_pool::session_handle::session_handle(session_handle&& other) noexcept
        : pool_(std::move(other.pool_))
        , sess_(std::move(other.sess_))
    {
    }

    connection_pool::session_handle&
    connection_pool::session_handle::operator=(session_handle&& other) noexcept
    {
        if (this != &other)
        {
            release();
            pool_ = std::move(other.pool_);
            sess_ = std::move(other.sess_);
        }
        return *this;
    }

    connection_pool::session_handle::~session_handle() { release(); }

    void
    connection_pool::session_handle::release()
    {
        auto pool = pool_.lock();
        if (pool && sess_)
        {
            pool->release_session(std::move(sess_));
        }
    }

    session*
    connection_pool::session_handle::get()
    {
        return sess_.get();
    }

    session const*
    connection_pool::session_handle::get() const
    {
        return sess_.get();
    }

    session*
    connection_pool::session_handle::operator->()
    {
        return get();
    }

    session const*
    connection_pool::session_handle::operator->() const
    {
        return get();
    }

    session&
    connection_pool::session_handle::operator*()
    {
        return *get();
    }

    session const&
    connection_pool::session_handle::operator*() const
    {
        return *get();
    }

    // ---- connection_pool ----

    connection_pool::connection_pool(net::any_io_executor ex, pool_params c, connect_fn connect)
        : impl_(std::make_shared<impl>(ex, std::move(c), std::move(connect)))
    {
    }

    connection_pool::~connection_pool() { stop(); }

    connection_pool::connection_pool(connection_pool&&) noexcept = default;
    connection_pool& connection_pool::operator=(connection_pool&&) noexcept = default;

    void
    connection_pool::start()
    {
        impl_->start();
    }

    net::awaitable<connection_pool::session_handle>
    connection_pool::async_acquire(std::chrono::steady_clock::duration wait_timeout)
    {
        co_return co_await impl_->async_acquire(wait_timeout);
    }

    void
    connection_pool::stop()
    {
        impl_->stop();
    }

    size_t
    connection_pool::active_count() const
    {
        return impl_->active_count();
    }

    size_t
    connection_pool::idle_count() const
    {
        return impl_->idle_count();
    }

    size_t
    connection_pool::total_count() const
    {
        return impl_->total_count();
    }

    net::any_io_executor
    connection_pool::get_executor() noexcept
    {
        return impl_->get_executor();
    }

    std::shared_ptr<spdlog::logger>
    connection_pool::logger() const
    {
        return impl_->logger();
    }

    void
    connection_pool::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        impl_->set_logger(std::move(logger));
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
