#ifdef HTTPLIB_ENABLED_DATABASE
#include "connection_pool_impl.h"
#include "httplib/db/exception.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "util/logging.hpp"
#include <boost/asio/co_spawn.hpp>
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
        : ticker(ex)
        , ex_(ex)
        , cfg_(std::move(cfg))
        , connect_(std::move(connect))
    {
        if (cfg_.min_connections > cfg_.max_connections)
        {
            cfg_.min_connections = cfg_.max_connections;
        }

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
        set_interval(check_interval);

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

    net::awaitable<bool>
    connection_pool::impl::on_start()
    {
        auto const epoch = epoch_.load();
        logger()->debug("db pool started");

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
            if (!is_running() || epoch_ != epoch)
            {
                co_return false;
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

        co_return true;
    }

    net::awaitable<connection_pool::session_handle>
    connection_pool::impl::async_acquire(std::chrono::steady_clock::duration wait_timeout)
    {
        if (!is_running())
        {
            throw pool_closed_error();
        }

        auto self = std::static_pointer_cast<impl>(shared_from_this());
        auto deadline = std::chrono::steady_clock::now() + wait_timeout;

        do
        {
            if (auto [sess, epoch] = co_await self->try_pop_validated(); sess)
            {
                co_return session_handle(self, std::move(sess), epoch);
            }

            std::unique_lock<std::mutex> lock(self->mutex_);
            if (!self->is_running())
            {
                throw pool_closed_error();
            }

            if (has_capacity_locked())
            {
                inc_active_locked();
                // 与计数同临界区取轮次样本：stop/重启期间建连成功后按旧轮丢弃。
                auto epoch = self->epoch_.load();
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
                    co_return session_handle(self, std::move(sess), epoch);
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lk(self->mutex_);
                    if (self->is_running() && self->epoch_.load() == epoch)
                    {
                        dec_active_locked();
                        // 本协程建连失败后槽位已释放，唤醒下一个等待者接手，避免其睡到超时。
                        self->wake_one_waiter();
                    }
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

        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->logger()->warn(
                "db pool acquire timed out after {}ms: active={} idle={} validating={} total={} max={}",
                std::chrono::duration_cast<std::chrono::milliseconds>(wait_timeout).count(),
                self->active_count_,
                self->idle_.size(),
                self->validating_,
                self->total_locked(),
                self->cfg_.max_connections);
        }
        throw pool_timeout_error();
    }

    void
    connection_pool::impl::release_session(std::unique_ptr<session> sess, uint64_t epoch)
    {
        if (!sess)
        {
            return;
        }

        sess->set_query_logger({});

        if (!sess->is_live())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (is_running() && epoch == epoch_)
            {
                dec_active_locked();
                wake_one_waiter();
            }
            return;
        }

        if (!sess->in_transaction())
        {
            push_idle(std::move(sess), epoch);
            return;
        }

        auto self = std::static_pointer_cast<impl>(shared_from_this());
        net::co_spawn(
            ex_,
            [self, sess = std::move(sess), epoch]() mutable -> net::awaitable<void>
            {
                try
                {
                    co_await sess->rollback();
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lk(self->mutex_);
                    if (self->is_running() && epoch == self->epoch_)
                    {
                        self->dec_active_locked();
                        self->wake_one_waiter();
                    }
                    co_return;
                }

                self->push_idle(std::move(sess), epoch);
            },
            [](std::exception_ptr) {});
    }

    void
    connection_pool::impl::stop()
    {
        std::vector<std::unique_ptr<session>> to_close;
        waiters_list waiters;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 递增 epoch：让 stop 前已在跑的维护协程在 await 之后识别出自己已过期。
            ++epoch_;

            waiters.swap(waiters_);
            to_close = std::move(idle_);

            // 与 http_client_pool 对齐：停止即清零计数；旧 handle 归还时因 epoch 不符被丢弃，
            // 不再改动重启后新池的计数。
            active_count_ = 0;
            validating_ = 0;
        }

        // 锁外请求 ticker 取消维护协程：避免持 mutex_ 时 inline 驱动 on_stop 造成重入/死锁。
        ticker::stop();

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

    net::awaitable<std::pair<std::unique_ptr<session>, uint64_t>>
    connection_pool::impl::try_pop_validated()
    {
        std::unique_ptr<session> sess;
        uint64_t epoch = 0;
        std::vector<std::unique_ptr<session>> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            epoch = epoch_.load();
            sess = try_pop_idle(discarded);
        }
        // 锁外析构被丢弃的死连接，避免池锁内进入后端 close。
        discarded.clear();

        if (!sess)
        {
            co_return std::make_pair(std::move(sess), epoch);
        }

        if (!cfg_.validate_on_borrow || co_await sess->ping())
        {
            // stop/重启后不能再把旧连接塞回新池，也不能改新池计数。
            if (!is_running() || epoch != epoch_)
            {
                sess.reset();
            }
            co_return std::make_pair(std::move(sess), epoch);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (is_running() && epoch == epoch_)
        {
            dec_active_locked();
            wake_one_waiter(); // 校验剔除死连接释放了槽位，唤醒等待者接手
        }
        co_return std::make_pair(std::unique_ptr<session> {}, epoch);
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
    connection_pool::impl::push_idle(std::unique_ptr<session> sess, uint64_t epoch)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // stop/重启后旧 handle 归还：直接丢弃会话，不改动新池计数。
        if (!is_running() || epoch != epoch_)
        {
            return;
        }
        dec_active_locked();
        if (sess)
        {
            sess->touch();
        }
        idle_.push_back(std::move(sess));
        wake_one_waiter();
    }

    net::awaitable<std::unique_ptr<session>>
    connection_pool::impl::create_session()
    {
        co_return co_await connect_(ex_);
    }

    net::awaitable<bool>
    connection_pool::impl::on_tick()
    {
        // 本轮取样；stop()/重启后 epoch_ 变化，await 之后即可判定本 tick 已过期。
        auto const epoch = epoch_.load();
        auto active = [this, epoch]() noexcept { return is_running() && epoch_ == epoch; };

        auto now = std::chrono::steady_clock::now();

        std::vector<std::unique_ptr<session>> to_ping;
        std::vector<std::unique_ptr<session>> to_close;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active())
            {
                co_return false;
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
            if (sess && active())
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
            if (alive && active() && has_capacity_locked())
            {
                idle_.push_back(std::move(sess));
                wake_one_waiter();
            }
            else if (active())
            {
                wake_one_waiter(); // 剔除死连接释放了槽位，唤醒等待者接手
            }
        }

        if (!active())
        {
            co_return false;
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
            if (!active())
            {
                co_return false;
            }
            try
            {
                auto sess = co_await create_session();
                if (sess)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // 建连期间 acquire 侧可能已把池补满：超容量时丢弃，避免总连接数突破 max_connections。
                    if (active() && has_capacity_locked())
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

        co_return true;
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
