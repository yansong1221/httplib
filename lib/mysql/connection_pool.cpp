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

    namespace
    {

        struct idle_entry
        {
            std::unique_ptr<session::impl> sess_impl;
            std::chrono::steady_clock::time_point idle_since;
        };

        struct waiter_node
        {
            net::steady_timer timer;

            explicit waiter_node(net::any_io_executor const& ex) : timer(ex) {}
        };

    } // namespace

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

                if (auto imp = self->try_pop_idle())
                {
                    auto sess = std::make_unique<session>(std::move(imp));
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

                auto node = std::make_shared<waiter_node>(self->ex_);
                node->timer.expires_at(deadline);
                self->waiters_.push_back(node);
                lock.unlock();

                boost::system::error_code ec;
                co_await node->timer.async_wait(util::net_awaitable[ec]);

                lock.lock();

                if (auto imp = self->try_pop_idle())
                {
                    auto sess = std::make_unique<session>(std::move(imp));
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

            if (!imp.in_transaction && imp.stmts_to_close.empty())
            {
                // 快速路径：无事务、无待关闭 stmt，同步入池
                imp.last_active = std::chrono::steady_clock::now();

                auto released_impl = std::make_unique<session::impl>();
                released_impl->conn = std::move(imp.conn);
                released_impl->live = imp.live;
                released_impl->last_active = imp.last_active;
                released_impl->last_ping = imp.last_ping;

                idle_entry entry;
                entry.sess_impl = std::move(released_impl);
                entry.idle_since = entry.sess_impl->last_active;

                push_idle(std::move(entry));
                return;
            }

            auto conn = std::move(imp.conn);
            bool in_transaction = imp.in_transaction;
            auto stmts = std::move(imp.stmts_to_close);

            auto self = shared_from_this();
            net::co_spawn(
                ex_,
                [self, conn = std::move(conn), in_transaction, stmts = std::move(stmts)]() mutable
                    -> net::awaitable<void>
                {
                    try
                    {
                        for (auto& st : stmts)
                        {
                            if (st.valid())
                            {
                                boost::mysql::diagnostics diag;
                                co_await conn->async_close_statement(st, diag, boost::asio::use_awaitable);
                            }
                        }

                        if (in_transaction)
                        {
                            boost::mysql::results r;
                            boost::mysql::diagnostics diag;
                            co_await conn->async_execute("ROLLBACK", r, diag, boost::asio::use_awaitable);
                        }
                    }
                    catch (...)
                    {
                        std::lock_guard<std::mutex> lk(self->mutex_);
                        if (self->active_count_ > 0)
                        {
                            --self->active_count_;
                        }
                        co_return;
                    }

                    idle_entry entry;
                    entry.sess_impl = std::make_unique<session::impl>();
                    entry.sess_impl->conn = std::move(conn);
                    entry.sess_impl->live = true;
                    entry.sess_impl->last_active = std::chrono::steady_clock::now();
                    entry.idle_since = entry.sess_impl->last_active;

                    self->push_idle(std::move(entry));
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
                    waiter->timer.cancel();
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
        std::unique_ptr<session::impl>
        try_pop_idle()
        {
            while (!idle_.empty())
            {
                auto entry = std::move(idle_.back());
                idle_.pop_back();

                if (!entry.sess_impl || !entry.sess_impl->conn || !entry.sess_impl->live)
                {
                    continue;
                }

                ++active_count_;
                return std::move(entry.sess_impl);
            }
            return nullptr;
        }

        void
        push_idle(idle_entry entry)
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (active_count_ > 0)
            {
                --active_count_;
            }

            if (stopped_)
            {
                return;
            }

            if (!waiters_.empty())
            {
                idle_.push_back(std::move(entry));
                auto w = std::move(waiters_.front());
                waiters_.pop_front();
                if (auto waiter = w.lock())
                {
                    waiter->timer.cancel();
                }
                return;
            }

            idle_.push_back(std::move(entry));
        }

        net::awaitable<std::unique_ptr<session::impl>>
        create_connection_impl()
        {
            auto conn = std::make_unique<boost::mysql::any_connection>(ex_);
            boost::mysql::connect_params params;
            params.server_address.emplace_host_and_port(cfg_.host, cfg_.port);
            params.username = cfg_.user;
            params.password = cfg_.password;
            if (!cfg_.database.empty())
            {
                params.database = cfg_.database;
            }
            params.ssl = cfg_.ssl ? boost::mysql::ssl_mode::enable : boost::mysql::ssl_mode::disable;
            params.multi_queries = true;

            co_await conn->async_connect(params, boost::asio::use_awaitable);

            auto imp = std::make_unique<session::impl>();
            imp->params = cfg_;
            imp->conn = std::move(conn);
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
            std::vector<idle_entry> pre_created;
            for (size_t i = 0; i < cfg_.min_connections; ++i)
            {
                try
                {
                    auto imp = co_await create_connection_impl();
                    if (imp)
                    {
                        idle_entry entry;
                        entry.sess_impl = std::move(imp);
                        entry.idle_since = std::chrono::steady_clock::now();
                        pre_created.push_back(std::move(entry));
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
                for (auto& entry : pre_created)
                {
                    idle_.push_back(std::move(entry));
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

                std::vector<idle_entry> to_ping;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopped_)
                    {
                        co_return;
                    }

                    for (auto it = idle_.begin(); it != idle_.end();)
                    {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->idle_since);

                        if (cfg_.idle_timeout.count() > 0 && elapsed >= cfg_.idle_timeout)
                        {
                            size_t remaining = idle_.size() - 1;
                            if (remaining > cfg_.min_connections || idle_.size() > cfg_.min_connections)
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

                for (auto& entry : to_ping)
                {
                    if (stopped_)
                    {
                        co_return;
                    }

                    bool alive = false;
                    if (entry.sess_impl && entry.sess_impl->conn)
                    {
                        try
                        {
                            co_await entry.sess_impl->conn->async_ping(boost::asio::use_awaitable);
                            entry.sess_impl->last_ping = std::chrono::steady_clock::now();
                            entry.sess_impl->last_active = entry.sess_impl->last_ping;
                            entry.sess_impl->live = true;
                            alive = true;
                        }
                        catch (...)
                        {
                            entry.sess_impl->live = false;
                        }
                    }

                    if (alive)
                    {
                        entry.idle_since = std::chrono::steady_clock::now();
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!stopped_)
                        {
                            idle_.push_back(std::move(entry));
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
                        auto imp = co_await create_connection_impl();
                        if (imp)
                        {
                            idle_entry entry;
                            entry.sess_impl = std::move(imp);
                            entry.idle_since = std::chrono::steady_clock::now();

                            std::lock_guard<std::mutex> lock(mutex_);
                            if (!stopped_)
                            {
                                idle_.push_back(std::move(entry));
                            }
                        }
                    }
                    catch (...)
                    {
                    }
                }
            }
        }

        using waiters_list = std::deque<std::weak_ptr<waiter_node>>;

        mutable std::mutex mutex_;
        std::vector<idle_entry> idle_;
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
