#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/connection_pool.hpp"
#include "httplib/mysql/session.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "mysql/session_impl.h"
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>
#include <deque>
#include <mutex>
#include <vector>

namespace httplib::mysql
{

    struct connection_pool::impl : public std::enable_shared_from_this<impl>
    {
        impl(net::any_io_executor ex, pool_params cfg) : ex_(ex), cfg_(std::move(cfg)), maintain_timer_(ex) {}

        ~impl() { stop(); }

        void
        start()
        {
            if (!stopped_.exchange(false))
            {
                return;
            }
            net::co_spawn(
                ex_,
                [this, self = shared_from_this()]() -> net::awaitable<void> { co_await co_init_and_maintain(); },
                [](std::exception_ptr e)
                {
                    if (e)
                    {
                        std::rethrow_exception(e);
                    }
                });
        }

        net::awaitable<session_handle>
        async_acquire(std::chrono::steady_clock::duration wait_timeout)
        {
            if (stopped_)
            {
                throw std::runtime_error("connection_pool: pool is shut down");
            }

            if (wait_timeout <= std::chrono::steady_clock::duration::zero())
            {
                wait_timeout = cfg_.acquire_timeout;
            }

            auto deadline = wait_timeout <= std::chrono::steady_clock::duration::zero()
                                ? std::chrono::steady_clock::time_point::max()
                                : std::chrono::steady_clock::now() + wait_timeout;

            auto self = shared_from_this();

            do
            {
                std::unique_lock<std::mutex> lock(self->mutex_);
                if (self->stopped_)
                {
                    throw std::runtime_error("connection_pool: pool is shut down");
                }

                if (auto sess = self->try_pop_idle())
                {
                    co_return session_handle(self, std::move(sess));
                }

                if (active_count_ + idle_.size() < cfg_.max_connections)
                {
                    ++active_count_;
                    lock.unlock();

                    try
                    {
                        auto sess = co_await self->create_session();
                        co_return session_handle(self, std::move(sess));
                    }
                    catch (...)
                    {
                        std::lock_guard<std::mutex> lk(self->mutex_);
                        if (active_count_ > 0)
                        {
                            --active_count_;
                        }
                        throw;
                    }
                }

                if (deadline == std::chrono::steady_clock::time_point::max())
                {
                    throw std::runtime_error("connection_pool: pool is full and no timeout specified");
                }
                if (deadline <= std::chrono::steady_clock::now())
                {
                    throw std::runtime_error("connection_pool: acquire timeout");
                }

                std::erase_if(self->waiters_, [](auto const& w) { return w.expired(); });

                auto node = std::make_shared<net::steady_timer>(self->ex_);
                node->expires_at(deadline);
                self->waiters_.push_back(node);
                lock.unlock();

                boost::system::error_code ec;
                co_await node->async_wait(util::net_awaitable[ec]);

                lock.lock();

                if (auto sess = self->try_pop_idle())
                {
                    co_return session_handle(self, std::move(sess));
                }
            } while (deadline > std::chrono::steady_clock::now());

            throw std::runtime_error("connection_pool: acquire timeout");
        }

        void
        release_session(std::unique_ptr<session> sess)
        {
            if (!sess)
            {
                return;
            }

            auto& imp = get_impl(*sess);
            imp.query_logger = {};

            if (!imp.live)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (active_count_ > 0)
                {
                    --active_count_;
                }
                wake_one_waiter();
                return;
            }

            if (!imp.in_transaction && imp.stmts_to_close.empty())
            {
                push_idle(std::move(sess));
                return;
            }

            auto self = shared_from_this();
            net::co_spawn(
                ex_,
                [self, sess = std::move(sess)]() mutable -> net::awaitable<void>
                {
                    auto& imp = get_impl(*sess);
                    auto stmts = std::move(imp.stmts_to_close);
                    bool in_transaction = imp.in_transaction;

                    try
                    {
                        for (auto& st : stmts)
                        {
                            if (st.valid())
                            {
                                boost::mysql::diagnostics diag;
                                co_await imp.get_conn().async_close_statement(st, diag, boost::asio::use_awaitable);
                            }
                        }

                        if (in_transaction)
                        {
                            co_await sess->rollback();
                        }
                    }
                    catch (...)
                    {
                        std::lock_guard<std::mutex> lk(self->mutex_);
                        if (self->active_count_ > 0)
                        {
                            --self->active_count_;
                        }
                        self->wake_one_waiter();
                        co_return;
                    }

                    self->push_idle(std::move(sess));
                },
                [](std::exception_ptr) {});
        }

        void
        stop()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopped_.exchange(true))
            {
                return;
            }
            maintain_timer_.cancel();

            waiters_list waiters;
            waiters.swap(waiters_);
            idle_.clear();

            lock.unlock();

            for (auto& w : waiters)
            {
                if (auto waiter = w.lock())
                {
                    waiter->cancel();
                }
            }
        }

        size_t
        active_count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return active_count_;
        }

        size_t
        idle_count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return idle_.size();
        }

        size_t
        total_count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return active_count_ + idle_.size();
        }

        net::any_io_executor
        get_executor() noexcept
        {
            return ex_;
        }

      private:
        std::unique_ptr<session>
        try_pop_idle()
        {
            while (!idle_.empty())
            {
                auto sess = std::move(idle_.back());
                idle_.pop_back();

                if (!sess)
                {
                    continue;
                }
                auto& imp = get_impl(*sess);
                if (!imp.conn || !imp.live)
                {
                    continue;
                }

                ++active_count_;
                return std::move(sess);
            }
            return nullptr;
        }

        void
        wake_one_waiter()
        {
            while (!waiters_.empty())
            {
                auto w = std::move(waiters_.front());
                waiters_.pop_front();
                if (auto waiter = w.lock())
                {
                    waiter->cancel();
                    return;
                }
            }
        }

        void
        push_idle(std::unique_ptr<session> sess)
        {
            if (sess)
            {
                get_impl(*sess).last_active = std::chrono::steady_clock::now();
            }

            std::lock_guard<std::mutex> lock(mutex_);

            if (active_count_ > 0)
            {
                --active_count_;
            }
            if (stopped_)
            {
                return;
            }
            idle_.push_back(std::move(sess));

            wake_one_waiter();
        }

        net::awaitable<std::unique_ptr<session::impl>>
        create_connection_impl()
        {
            auto conn = std::make_unique<boost::mysql::any_connection>(ex_);
            auto offset = co_await connect_session(*conn, cfg_);

            auto imp = std::make_unique<session::impl>();
            imp->params = cfg_;
            imp->conn = std::move(conn);
            imp->utc_offset = offset;
            imp->stmt_cache.capacity = cfg_.max_cached_statements;
            co_return imp;
        }

        net::awaitable<std::unique_ptr<session>>
        create_session()
        {
            auto imp = co_await create_connection_impl();
            co_return std::make_unique<session>(std::move(imp));
        }

        net::awaitable<void>
        co_init_and_maintain()
        {
            std::vector<std::unique_ptr<session>> pre_created;
            for (size_t i = 0; i < cfg_.min_connections; ++i)
            {
                try
                {
                    auto sess = co_await create_session();
                    if (sess)
                    {
                        pre_created.push_back(std::move(sess));
                    }
                }
                catch (...)
                {
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopped_)
                {
                    co_return;
                }
                for (auto& sess : pre_created)
                {
                    idle_.push_back(std::move(sess));
                }
            }

            co_await co_maintain();
            co_return;
        }

        net::awaitable<void>
        co_maintain()
        {
            auto check_interval
                = cfg_.idle_check_interval.count() > 0 ? cfg_.idle_check_interval : std::chrono::seconds(60);

            boost::system::error_code ec;
            while (!stopped_)
            {
                maintain_timer_.expires_after(check_interval);
                co_await maintain_timer_.async_wait(util::net_awaitable[ec]);
                if (ec || stopped_)
                {
                    co_return;
                }

                auto now = std::chrono::steady_clock::now();

                std::vector<std::unique_ptr<session>> to_ping;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopped_)
                    {
                        co_return;
                    }

                    for (auto it = idle_.begin(); it != idle_.end();)
                    {
                        auto elapsed
                            = std::chrono::duration_cast<std::chrono::seconds>(now - get_impl(**it).last_active);

                        if (cfg_.idle_timeout.count() > 0 && elapsed >= cfg_.idle_timeout)
                        {
                            if (idle_.size() > cfg_.min_connections)
                            {
                                it = idle_.erase(it);
                                continue;
                            }
                        }

                        if (cfg_.health_check_interval.count() > 0 && elapsed >= cfg_.health_check_interval)
                        {
                            to_ping.push_back(std::move(*it));
                            it = idle_.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }

                for (auto& sess : to_ping)
                {
                    if (stopped_)
                    {
                        co_return;
                    }

                    bool alive = false;
                    if (sess)
                    {
                        auto& imp = get_impl(*sess);
                        if (imp.conn)
                        {
                            try
                            {
                                co_await imp.conn->async_ping(boost::asio::use_awaitable);
                                imp.last_ping = std::chrono::steady_clock::now();
                                imp.last_active = imp.last_ping;
                                imp.live = true;
                                alive = true;
                            }
                            catch (...)
                            {
                                imp.live = false;
                            }
                        }
                    }

                    if (alive)
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!stopped_)
                        {
                            idle_.push_back(std::move(sess));
                        }
                    }
                }

                size_t deficit = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    size_t total = idle_.size() + active_count_;
                    if (total < cfg_.min_connections)
                    {
                        deficit = cfg_.min_connections - total;
                    }
                }

                for (size_t i = 0; i < deficit; ++i)
                {
                    if (stopped_)
                    {
                        co_return;
                    }
                    try
                    {
                        auto sess = co_await create_session();
                        if (sess)
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (!stopped_)
                            {
                                idle_.push_back(std::move(sess));
                            }
                        }
                    }
                    catch (...)
                    {
                    }
                }
            }
        }

        using waiters_list = std::deque<std::weak_ptr<net::steady_timer>>;

        mutable std::mutex mutex_;
        std::vector<std::unique_ptr<session>> idle_;
        size_t active_count_ = 0;
        waiters_list waiters_;

        net::any_io_executor ex_;
        pool_params cfg_;

        std::atomic<bool> stopped_ { true };
        net::steady_timer maintain_timer_;
    };

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

    connection_pool::connection_pool(net::any_io_executor ex, pool_params c)
        : impl_(std::make_shared<impl>(ex, std::move(c)))
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

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
