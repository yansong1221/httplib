#ifdef HTTPLIB_ENABLED_DATABASE
#include "connection_pool_impl.h"
#include "httplib/db/exception.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "util/logging.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/errc.hpp>
#include <spdlog/spdlog.h>

namespace httplib::db
{
    namespace
    {
        /// 池已停止（或从未启动）。与 client::http_client_pool 的 operation_canceled 语义一致，
        /// 便于调用方按 code() 统一分流，而不必匹配错误消息。
        db_exception
        pool_closed_error()
        {
            return db_exception(boost::system::errc::make_error_code(boost::system::errc::operation_canceled),
                                "connection_pool: pool is shut down");
        }

        /// 借出等待超时（含 fail-fast 未等到连接）。与 client::http_client_pool 的 timed_out 一致。
        db_exception
        pool_timeout_error()
        {
            return db_exception(boost::system::errc::make_error_code(boost::system::errc::timed_out),
                                "connection_pool: acquire timeout");
        }
    } // namespace

    connection_pool::impl::impl(net::any_io_executor ex, pool_params cfg, connection_pool::connect_fn connect)
        : ex_(ex)
        , cfg_(std::move(cfg))
        , connect_(std::move(connect))
        , maintain_timer_(ex)
    {
        if (cfg_.min_connections > cfg_.max_connections)
        {
            cfg_.min_connections = cfg_.max_connections;
        }

        default_logger_ = httplib::detail::make_console_logger("httplib.db_pool");
    }

    connection_pool::impl::~impl() { stop(); }

    std::shared_ptr<spdlog::logger>
    connection_pool::impl::logger() const
    {
        auto l = custom_logger_.load();
        return l ? l : default_logger_;
    }

    void
    connection_pool::impl::set_logger(std::shared_ptr<spdlog::logger> l)
    {
        custom_logger_.store(std::move(l));
    }

    void
    connection_pool::impl::start()
    {
        if (!stopped_.exchange(false))
        {
            return;
        }
        auto epoch = epoch_.load();
        net::co_spawn(
            ex_,
            [this, self = shared_from_this(), epoch]() -> net::awaitable<void>
            { co_await co_init_and_maintain(epoch); },
            [self = shared_from_this()](std::exception_ptr e)
            {
                if (e)
                {
                    try
                    {
                        std::rethrow_exception(e);
                    }
                    catch (std::exception const& ex)
                    {
                        self->logger()->error("db pool maintenance failed: {}", ex.what());
                    }
                    catch (...)
                    {
                        self->logger()->error("db pool maintenance failed: unknown error");
                    }
                }
            });
    }

    net::awaitable<connection_pool::session_handle>
    connection_pool::impl::async_acquire(std::chrono::steady_clock::duration wait_timeout)
    {
        if (stopped_)
        {
            throw pool_closed_error();
        }

        auto self = shared_from_this();
        auto deadline = std::chrono::steady_clock::now() + wait_timeout;

        do
        {
            if (auto sess = co_await self->try_pop_validated())
            {
                co_return session_handle(self, std::move(sess));
            }

            std::unique_lock<std::mutex> lock(self->mutex_);
            if (self->stopped_)
            {
                throw pool_closed_error();
            }

            if (has_capacity_locked())
            {
                inc_active_locked();
                lock.unlock();

                try
                {
                    auto sess = co_await self->create_session();
                    if (!sess)
                    {
                        // 工厂返回空会话视为建连失败，释放槽位并唤醒下一个等待者。
                        throw db_exception(
                            boost::system::errc::make_error_code(boost::system::errc::connection_aborted),
                            "connection_pool: session factory returned an empty session");
                    }
                    co_return session_handle(self, std::move(sess));
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lk(self->mutex_);
                    dec_active_locked();
                    // 本协程建连失败后槽位已释放，唤醒下一个等待者接手，避免其睡到超时。
                    self->wake_one_waiter();
                    throw;
                }
            }

            if (wait_timeout <= std::chrono::steady_clock::duration::zero())
            {
                throw pool_timeout_error();
            }

            std::erase_if(self->waiters_, [](auto const& w) { return w.expired(); });

            auto node = std::make_shared<net::steady_timer>(self->ex_);
            node->expires_at(deadline);
            self->waiters_.push_back(node);
            lock.unlock();

            boost::system::error_code ec;
            co_await node->async_wait(util::net_awaitable[ec]);
        } while (deadline > std::chrono::steady_clock::now());

        throw pool_timeout_error();
    }

    void
    connection_pool::impl::release_session(std::unique_ptr<session> sess)
    {
        if (!sess)
        {
            return;
        }

        sess->set_query_logger({});

        if (!sess->is_live())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dec_active_locked();
            wake_one_waiter();
            return;
        }

        if (!sess->in_transaction())
        {
            push_idle(std::move(sess));
            return;
        }

        auto self = shared_from_this();
        net::co_spawn(
            ex_,
            [self, sess = std::move(sess)]() mutable -> net::awaitable<void>
            {
                try
                {
                    co_await sess->rollback();
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lk(self->mutex_);
                    self->dec_active_locked();
                    self->wake_one_waiter();
                    co_return;
                }

                self->push_idle(std::move(sess));
            },
            [](std::exception_ptr) {});
    }

    void
    connection_pool::impl::stop()
    {
        std::vector<std::unique_ptr<session>> to_close;
        waiters_list waiters;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopped_.exchange(true))
            {
                return;
            }
            epoch_.fetch_add(1);
            maintain_timer_.cancel();

            waiters.swap(waiters_);
            to_close = std::move(idle_);
        }

        for (auto& w : waiters)
        {
            if (auto waiter = w.lock())
            {
                waiter->cancel();
            }
        }
        // to_close 在函数返回时于锁外析构（session 析构可能进入后端 close）。
    }

    size_t
    connection_pool::impl::active_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_count_;
    }

    size_t
    connection_pool::impl::idle_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return idle_.size();
    }

    size_t
    connection_pool::impl::total_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_locked();
    }

    std::unique_ptr<session>
    connection_pool::impl::try_pop_idle(std::vector<std::unique_ptr<session>>& discarded)
    {
        while (!idle_.empty())
        {
            auto sess = std::move(idle_.back());
            idle_.pop_back();

            if (!sess)
            {
                continue;
            }
            if (!sess->is_live())
            {
                // 死连接移出 idle 后交给调用方在锁外析构。
                discarded.push_back(std::move(sess));
                continue;
            }

            inc_active_locked();
            return std::move(sess);
        }
        return nullptr;
    }

    net::awaitable<std::unique_ptr<session>>
    connection_pool::impl::try_pop_validated()
    {
        std::unique_ptr<session> sess;
        std::vector<std::unique_ptr<session>> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sess = try_pop_idle(discarded);
        }
        // 锁外析构被丢弃的死连接，避免池锁内进入后端 close。
        discarded.clear();

        if (!sess)
        {
            co_return nullptr;
        }

        if (!cfg_.validate_on_borrow || co_await sess->ping())
        {
            co_return sess;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        dec_active_locked();
        wake_one_waiter(); // 校验剔除死连接释放了槽位，唤醒等待者接手
        co_return nullptr;
    }

    void
    connection_pool::impl::wake_one_waiter()
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
    connection_pool::impl::push_idle(std::unique_ptr<session> sess)
    {
        if (sess)
        {
            sess->touch();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        dec_active_locked();
        if (stopped_)
        {
            return;
        }
        idle_.push_back(std::move(sess));
        wake_one_waiter();
    }

    net::awaitable<std::unique_ptr<session>>
    connection_pool::impl::create_session()
    {
        co_return co_await connect_(ex_);
    }

    net::awaitable<void>
    connection_pool::impl::co_init_and_maintain(uint64_t epoch)
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
            catch (std::exception const& ex)
            {
                logger()->warn("db pool pre-create connection failed: {}", ex.what());
            }
            catch (...)
            {
                logger()->warn("db pool pre-create connection failed: unknown error");
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_ || epoch_ != epoch)
            {
                co_return;
            }
            // 预建期间 acquire 侧可能已把池补满：超容量时丢弃剩余预建连接，避免突破 max_connections。
            for (auto& sess : pre_created)
            {
                if (!has_capacity_locked())
                {
                    break;
                }
                idle_.push_back(std::move(sess));
            }
        }

        co_await co_maintain(epoch);
        co_return;
    }

    net::awaitable<void>
    connection_pool::impl::co_maintain(uint64_t epoch)
    {
        // 维护周期取各启用 interval 的最小值：健康检查实际受循环 tick 粒度钳制，
        // 取 min 保证每个旋钮都能按其声明的间隔生效（两者都禁用时退回 60s）。
        std::chrono::steady_clock::duration check_interval = std::chrono::seconds(60);
        if (cfg_.idle_check_interval.count() > 0)
        {
            check_interval = std::min(check_interval, cfg_.idle_check_interval);
        }
        if (cfg_.health_check_interval.count() > 0)
        {
            check_interval = std::min(check_interval, cfg_.health_check_interval);
        }

        boost::system::error_code ec;
        while (!stopped_ && epoch_ == epoch)
        {
            maintain_timer_.expires_after(check_interval);
            co_await maintain_timer_.async_wait(util::net_awaitable[ec]);
            if (ec || stopped_ || epoch_ != epoch)
            {
                co_return;
            }

            auto now = std::chrono::steady_clock::now();

            std::vector<std::unique_ptr<session>> to_ping;
            std::vector<std::unique_ptr<session>> to_close;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopped_)
                {
                    co_return;
                }

                for (auto it = idle_.begin(); it != idle_.end();)
                {
                    auto last_active = (*it)->last_active_time();
                    auto last_ping = (*it)->last_ping_time();
                    // 保持亚秒精度比较（配置周期为 steady_clock::duration），不再向下取整到秒。
                    auto idle_elapsed = now - last_active;
                    auto since_ping = now - last_ping;

                    if (cfg_.idle_timeout.count() > 0 && idle_elapsed >= cfg_.idle_timeout)
                    {
                        if (idle_.size() > cfg_.min_connections)
                        {
                            to_close.push_back(std::move(*it));
                            it = idle_.erase(it);
                            wake_one_waiter(); // 空闲回收释放了槽位，唤醒等待者避免其睡到超时
                            continue;
                        }
                    }

                    if (cfg_.health_check_interval.count() > 0 && since_ping >= cfg_.health_check_interval)
                    {
                        to_ping.push_back(std::move(*it));
                        it = idle_.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }

                // 拉去健康检查的连接仍占容量（见 validating_ 注释）。必须在同一临界区内计数，
                // 与借出侧的容量判断互斥，杜绝验证窗口内的超建。
                validating_ += to_ping.size();
            }

            // 锁外析构被空闲回收的连接（session 析构可能进入后端 close）。
            to_close.clear();

            for (auto& sess : to_ping)
            {
                // stop()/重启期间不再发 ping，但仍要走下面的归还/剔除记账，
                // 保证 validating_ 无论何种退出路径都精确归零。
                bool alive = false;
                if (sess && !stopped_ && epoch_ == epoch)
                {
                    try
                    {
                        alive = co_await sess->ping();
                    }
                    catch (...)
                    {
                        // 后端 ping 未承诺不抛：异常视同连接失效。既保证 validating_ 精确归零，
                        // 也不让单个坏连接中断整个维护协程。
                        alive = false;
                    }
                }

                std::lock_guard<std::mutex> lock(mutex_);
                if (validating_ > 0)
                {
                    --validating_;
                }
                // 健康检查期间池子可能已被新连接补满：超容量时直接关闭刚检查完的连接，
                // 避免总连接数短暂突破 max_connections 后迟迟不回收。
                if (alive && !stopped_ && epoch_ == epoch && has_capacity_locked())
                {
                    idle_.push_back(std::move(sess));
                    wake_one_waiter();
                }
                else if (!stopped_ && epoch_ == epoch)
                {
                    wake_one_waiter(); // 剔除死连接释放了槽位，唤醒等待者接手
                }
            }

            if (stopped_ || epoch_ != epoch)
            {
                co_return;
            }

            size_t deficit = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // 验证中的连接计入总数，否则健康检查窗口内会误判缺额而超建。
                if (size_t total = total_locked(); total < cfg_.min_connections)
                {
                    deficit = cfg_.min_connections - total;
                }
            }

            for (size_t i = 0; i < deficit; ++i)
            {
                if (stopped_ || epoch_ != epoch)
                {
                    co_return;
                }
                try
                {
                    auto sess = co_await create_session();
                    if (sess)
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        // 建连期间 acquire 侧可能已把池补满：超容量时丢弃，避免总连接数突破 max_connections。
                        if (!stopped_ && epoch_ == epoch && has_capacity_locked())
                        {
                            idle_.push_back(std::move(sess));
                            wake_one_waiter();
                        }
                    }
                }
                catch (std::exception const& ex)
                {
                    logger()->warn("db pool refill connection failed: {}", ex.what());
                }
                catch (...)
                {
                    logger()->warn("db pool refill connection failed: unknown error");
                }
            }
        }
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
