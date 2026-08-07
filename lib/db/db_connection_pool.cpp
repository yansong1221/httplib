#ifdef HTTPLIB_ENABLED_DATABASE
#    include "httplib/db/db_connection_pool.hpp"
#    include <boost/asio/redirect_error.hpp>
#    include <boost/asio/use_awaitable.hpp>
#    include <stdexcept>

namespace httplib::db
{

    db_connection_pool::db_connection_pool(net::any_io_executor ex, db_config config, db_connection_factory factory)
        : ex_(std::move(ex))
        , config_(std::move(config))
        , factory_(std::move(factory))
    {
    }

    db_connection_pool::db_connection_pool(boost::asio::io_context& io_ctx,
                                           db_config config,
                                           db_connection_factory factory)
        : db_connection_pool(io_ctx.get_executor(), std::move(config), std::move(factory))
    {
    }

    db_connection_pool::~db_connection_pool() { shutdown_.store(true, std::memory_order_relaxed); }

    net::awaitable<void>
    db_connection_pool::init()
    {
        if (initialized_)
        {
            throw std::logic_error("db_connection_pool::init() called more than once");
        }
        initialized_ = true;

        for (size_t i = 0; i < config_.min_connections; ++i)
        {
            auto conn = co_await factory_(ex_, config_);
            if (conn)
            {
                conn->touch();
                std::lock_guard lock(mutex_);
                idle_.push_back(std::move(conn));
            }
        }

        if (config_.idle_check_interval.count() > 0)
        {
            start_idle_checker();
        }

        if (config_.health_check_interval.count() > 0)
        {
            start_health_checker();
        }
    }

    net::awaitable<std::shared_ptr<db_connection>>
    db_connection_pool::acquire()
    {
        if (shutdown_.load(std::memory_order_relaxed))
        {
            throw std::runtime_error("db_connection_pool: pool is shut down");
        }

        std::unique_lock lock(mutex_);
        std::vector<std::shared_ptr<db_connection>> dead_conns;

        while (!idle_.empty())
        {
            auto conn = std::move(idle_.back());
            idle_.pop_back();

            if (!conn->is_alive())
            {
                dead_conns.push_back(std::move(conn));
                continue;
            }

            ++active_count_;
            lock.unlock();
            dead_conns.clear();

            bool skip_ping = config_.health_check_interval.count() > 0 && config_.ping_grace_period.count() > 0;
            if (skip_ping)
            {
                auto since_last_ping = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
                                                                                        - conn->last_ping_time());
                if (since_last_ping < config_.ping_grace_period)
                {
                    conn->touch();
                    co_return conn;
                }
            }

            bool alive = false;
            try
            {
                alive = co_await conn->ping();
            }
            catch (...)
            {
                alive = false;
            }

            if (alive)
            {
                conn->touch();
                co_return conn;
            }

            lock.lock();
            --active_count_;
        }

        if (active_count_ + idle_.size() < config_.max_connections)
        {
            ++active_count_;
            lock.unlock();

            try
            {
                auto conn = co_await factory_(ex_, config_);
                conn->touch();
                co_return conn;
            }
            catch (...)
            {
                std::lock_guard rollback_lk(mutex_);
                --active_count_;
                throw;
            }
        }

        auto timer = std::make_shared<boost::asio::steady_timer>(ex_, config_.acquire_timeout);
        auto result = std::make_shared<std::shared_ptr<db_connection>>();
        waiters_.push_back({ timer, result });
        lock.unlock();

        boost::system::error_code ec;
        co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (*result)
        {
            std::lock_guard count_lock(mutex_);
            ++active_count_;
            (*result)->touch();
            co_return std::move(*result);
        }

        bool removed = false;
        {
            std::lock_guard clean_lock(mutex_);
            auto before = waiters_.size();
            std::erase_if(waiters_, [&timer](waiter const& w) { return w.timer == timer; });
            removed = (waiters_.size() < before);
        }

        if (!removed && *result)
        {
            std::lock_guard count_lock(mutex_);
            ++active_count_;
            (*result)->touch();
            co_return std::move(*result);
        }

        if (*result)
        {
            release(std::move(*result));
        }

        throw std::runtime_error("db_connection_pool: acquire timeout");
    }

    void
    db_connection_pool::release(std::shared_ptr<db_connection> conn)
    {
        if (!conn)
        {
            return;
        }

        if (conn->in_transaction())
        {
            auto conn_ptr = std::move(conn);
            auto self = shared_from_this();
            net::co_spawn(
                ex_,
                [self, conn_ptr]() mutable -> net::awaitable<void>
                {
                    try
                    {
                        co_await conn_ptr->rollback();
                    }
                    catch (...)
                    {
                        std::lock_guard lock(self->mutex_);
                        if (self->active_count_ > 0)
                        {
                            --self->active_count_;
                        }
                        co_return;
                    }
                    conn_ptr->touch();
                    std::lock_guard lock(self->mutex_);
                    if (self->active_count_ > 0)
                    {
                        --self->active_count_;
                    }
                    if (self->shutdown_.load(std::memory_order_relaxed))
                    {
                        co_return;
                    }
                    if (!self->waiters_.empty())
                    {
                        self->wake_one_waiter(std::move(conn_ptr));
                    }
                    else
                    {
                        self->idle_.push_back(std::move(conn_ptr));
                    }
                },
                net::detached);
            return;
        }

        std::lock_guard lock(mutex_);

        if (active_count_ > 0)
        {
            --active_count_;
        }

        if (!waiters_.empty())
        {
            auto w = std::move(waiters_.front());
            waiters_.pop_front();
            *(w.result) = std::move(conn);
            w.timer->cancel();
            return;
        }

        if (!shutdown_.load(std::memory_order_relaxed))
        {
            conn->touch();
            idle_.push_back(std::move(conn));
        }
    }

    net::awaitable<void>
    db_connection_pool::shutdown()
    {
        shutdown_.store(true, std::memory_order_relaxed);

        if (idle_check_timer_)
        {
            idle_check_timer_->cancel();
        }
        if (health_check_timer_)
        {
            health_check_timer_->cancel();
        }

        std::lock_guard lock(mutex_);

        for (auto& w : waiters_)
        {
            w.timer->cancel();
        }
        waiters_.clear();

        idle_.clear();

        co_return;
    }

    size_t
    db_connection_pool::active_count() const
    {
        std::lock_guard lock(mutex_);
        return active_count_;
    }

    size_t
    db_connection_pool::idle_count() const
    {
        std::lock_guard lock(mutex_);
        return idle_.size();
    }

    size_t
    db_connection_pool::waiting_count() const
    {
        std::lock_guard lock(mutex_);
        return waiters_.size();
    }

    size_t
    db_connection_pool::total_count() const
    {
        std::lock_guard lock(mutex_);
        return active_count_ + idle_.size();
    }

    void
    db_connection_pool::wake_one_waiter(std::shared_ptr<db_connection> conn)
    {
        if (!waiters_.empty())
        {
            auto w = std::move(waiters_.front());
            waiters_.pop_front();
            *(w.result) = std::move(conn);
            w.timer->cancel();
        }
    }

    void
    db_connection_pool::start_idle_checker()
    {
        auto self = shared_from_this();
        net::co_spawn(ex_, [self]() -> net::awaitable<void> { co_await self->idle_check_loop(); }, net::detached);
    }

    net::awaitable<void>
    db_connection_pool::idle_check_loop()
    {
        idle_check_timer_ = std::make_shared<boost::asio::steady_timer>(ex_);
        while (!shutdown_.load(std::memory_order_relaxed))
        {
            std::chrono::steady_clock::time_point next_expire;
            {
                std::lock_guard lock(mutex_);
                if (idle_.empty())
                {
                    next_expire = std::chrono::steady_clock::now() + config_.idle_check_interval;
                }
                else
                {
                    auto oldest = std::min_element(idle_.begin(),
                                                   idle_.end(),
                                                   [](auto const& a, auto const& b)
                                                   { return a->last_active_time() < b->last_active_time(); });
                    next_expire = (*oldest)->last_active_time() + config_.idle_timeout;

                    auto max_wait = std::chrono::steady_clock::now() + config_.idle_check_interval;
                    if (next_expire > max_wait)
                    {
                        next_expire = max_wait;
                    }
                }
            }

            idle_check_timer_->expires_at(next_expire);
            boost::system::error_code ec;
            co_await idle_check_timer_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (shutdown_.load(std::memory_order_relaxed))
            {
                break;
            }

            auto now = std::chrono::steady_clock::now();
            std::lock_guard lock(mutex_);

            size_t remaining = idle_.size();
            for (auto it = idle_.begin(); it != idle_.end();)
            {
                if (remaining <= config_.min_connections)
                {
                    break;
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - (*it)->last_active_time());
                if (elapsed >= config_.idle_timeout)
                {
                    it = idle_.erase(it);
                    --remaining;
                }
                else
                {
                    ++it;
                }
            }
        }
    }

    void
    db_connection_pool::start_health_checker()
    {
        auto self = shared_from_this();
        net::co_spawn(ex_, [self]() -> net::awaitable<void> { co_await self->health_check_loop(); }, net::detached);
    }

    net::awaitable<void>
    db_connection_pool::health_check_loop()
    {
        health_check_timer_ = std::make_shared<boost::asio::steady_timer>(ex_);
        while (!shutdown_.load(std::memory_order_relaxed))
        {
            health_check_timer_->expires_after(config_.health_check_interval);
            boost::system::error_code ec;
            co_await health_check_timer_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (shutdown_.load(std::memory_order_relaxed))
            {
                break;
            }

            std::vector<std::shared_ptr<db_connection>> checking;
            {
                std::lock_guard lock(mutex_);
                checking = std::move(idle_);
                idle_.clear();
                active_count_ += checking.size();
            }

            std::vector<std::shared_ptr<db_connection>> alive;
            alive.reserve(checking.size());
            for (auto& conn : checking)
            {
                if (shutdown_.load(std::memory_order_relaxed))
                {
                    std::lock_guard lock(mutex_);
                    for (auto& c : alive)
                    {
                        idle_.push_back(std::move(c));
                    }
                    if (active_count_ >= checking.size())
                    {
                        active_count_ -= checking.size();
                    }
                    else
                    {
                        active_count_ = 0;
                    }
                    co_return;
                }
                bool ok = false;
                try
                {
                    ok = co_await conn->ping();
                }
                catch (...)
                {
                    ok = false;
                }
                if (ok)
                {
                    alive.push_back(std::move(conn));
                }
            }

            size_t deficit = 0;
            {
                std::lock_guard lock(mutex_);
                if (active_count_ >= checking.size())
                {
                    active_count_ -= checking.size();
                }
                else
                {
                    active_count_ = 0;
                }
                for (auto& conn : alive)
                {
                    idle_.push_back(std::move(conn));
                }
                size_t total = idle_.size() + active_count_;
                if (total < config_.min_connections)
                {
                    deficit = config_.min_connections - total;
                }
            }

            for (size_t i = 0; i < deficit; ++i)
            {
                if (shutdown_.load(std::memory_order_relaxed))
                {
                    co_return;
                }
                try
                {
                    auto new_conn = co_await factory_(ex_, config_);
                    if (new_conn)
                    {
                        new_conn->touch();
                        std::lock_guard lock(mutex_);
                        idle_.push_back(std::move(new_conn));
                    }
                }
                catch (...)
                {
                }
            }
        }
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
