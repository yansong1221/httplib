#ifdef HTTPLIB_ENABLED_DATABASE
#include "connection_pool_impl.h"
#include "httplib/db/exception.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "registry.hpp"
#include "util/logging.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/errc.hpp>
#include <spdlog/spdlog.h>

namespace httplib::db
{
    namespace
    {
        /// 池已停止。与 client::http_client_pool 的 operation_canceled 语义一致，
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
        : ticker(net::make_strand(ex))
        , base_executor_(ex)
        , cfg_(std::move(cfg))
        , connect_(std::move(connect))
        , httplib::detail::logger("httplib.db_pool")
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
    }

    connection_pool::impl::~impl() {}

    net::awaitable<bool>
    connection_pool::impl::on_start()
    {
        get_logger()->debug("db pool started");

        // on_start 与后续池状态访问都在池自身的 strand 上，无需加锁。
        // 预建连接在建立期间以 inflight_ 占容量（与 acquire_or_create 的 active 占位
        // 语义一致，但不出现在对外计数里），启动窗口内并发 async_acquire 才能看到
        // 真实容量余量，避免超建（也避免误以为有空闲而空等）。
        std::vector<std::unique_ptr<session>> pre_created;
        for (size_t i = 0; i < cfg_.min_connections && has_capacity(); ++i)
        {
            if (!is_running())
            {
                break;
            }
            ++inflight_;
            bool created = false;
            try
            {
                auto sess = co_await create_session();
                if (sess)
                {
                    pre_created.push_back(std::move(sess));
                    created = true;
                }
            }
            catch (boost::system::system_error const& e)
            {
                // 维护协程被 stop() 取消时，挂起的建连以 operation_aborted 抛出；
                // 据此区分"调用方主动停机"与"真实建连失败"，仅在真实失败时告警。
                auto ec = e.code();
                if (ec != net::error::operation_aborted)
                {
                    get_logger()->warn("db pool pre-create connection failed: {}", ec.message());
                }
            }
            catch (std::exception const& ex)
            {
                get_logger()->warn("db pool pre-create connection failed: {}", ex.what());
            }
            catch (...)
            {
                get_logger()->warn("db pool pre-create connection failed: unknown error");
            }

            // 空会话/建连失败的预建不再占容量；期间可能已有 acquire 因容量占满而排队，
            // 这里唤醒队首接管刚释放的容量，避免其睡到超时。
            if (!created)
            {
                --inflight_;
                wake_one_waiter();
            }
        }

        if (!is_running())
        {
            co_return false;
        }

        // 预建连接从 inflight_ 占位转为 idle；若期间 acquire 侧已把池补满（超容量）
        // 则丢弃剩余预建连接，避免突破 max_connections。每释放一个预建都唤醒队首。
        for (auto& sess : pre_created)
        {
            --inflight_;
            if (has_capacity())
            {
                idle_.push_back(std::move(sess));
                ++idle_metric_;
            }
            wake_one_waiter();
        }

        co_return true;
    }

    net::awaitable<connection_pool::session_handle>
    connection_pool::impl::async_acquire(std::chrono::steady_clock::duration wait_timeout)
    {

        auto self = std::static_pointer_cast<impl>(shared_from_this());

        // 池状态只在自身 strand 上访问：把调用协程切到 strand 后再操作，
        // 后续 await（校验/等待唤醒）都会在 strand 上恢复，因此无需再加锁。
        co_return co_await net::co_spawn(
            get_executor(),
            [&]() -> net::awaitable<connection_pool::session_handle>
            {
                if (!is_running())
                {
                    throw pool_closed_error();
                }

                auto deadline = std::chrono::steady_clock::now() + wait_timeout;

                std::shared_ptr<waiter_node> node;
                bool in_queue = false;
                /// 上一轮等待是否被池唤醒（而非超时）；被唤醒者即队首，可绕过公平性检查。
                bool serving = false;

                do
                {
                    if (!is_running())
                    {
                        if (in_queue)
                        {
                            remove_waiter(node);
                        }
                        throw pool_closed_error();
                    }

                    if (in_queue && deadline <= std::chrono::steady_clock::now())
                    {
                        remove_waiter(node);
                        get_logger()->warn(
                            "db pool acquire timed out after {}ms: active={} idle={} validating={} total={} max={}",
                            std::chrono::duration_cast<std::chrono::milliseconds>(wait_timeout).count(),
                            active_metric_.load(std::memory_order_relaxed),
                            idle_.size(),
                            validating_metric_.load(std::memory_order_relaxed),
                            total_size(),
                            cfg_.max_connections);
                        throw pool_timeout_error();
                    }

                    std::unique_ptr<session> sess;
                    try
                    {
                        sess = co_await acquire_or_create(serving);
                    }
                    catch (...)
                    {
                        // 建连/校验路径抛异常时，必须先把当前 waiter 的回合交还队列，
                        // 否则后续 waiter 可能因队首失效而迟迟不被唤醒。
                        if (in_queue)
                        {
                            remove_waiter(node);
                        }
                        throw;
                    }

                    if (sess)
                    {
                        if (in_queue)
                        {
                            remove_waiter(node);
                        }
                        co_return session_handle(self, std::move(sess));
                    }

                    serving = false;

                    if (wait_timeout <= std::chrono::steady_clock::duration::zero())
                    {
                        if (in_queue)
                        {
                            remove_waiter(node);
                        }
                        throw pool_timeout_error();
                    }

                    std::erase_if(waiters_, [](auto const& w) { return w.expired(); });

                    if (!in_queue)
                    {
                        node = std::make_shared<waiter_node>(get_executor());
                        waiters_.push_back(node);
                        in_queue = true;
                    }

                    auto remaining = deadline - std::chrono::steady_clock::now();
                    if (remaining <= std::chrono::steady_clock::duration::zero())
                    {
                        // 截止时间已到，交给下一轮循环顶部的超时判断处理。
                        continue;
                    }

                    auto result = co_await node->event.wait_for(remaining);
                    serving = (result == util::async_event::wait_result::notified);

                } while (true);
            },
            net::use_awaitable);
    }

    void
    connection_pool::impl::release_session(std::unique_ptr<session> sess)
    {
        // 归还可能来自任意线程（session_handle 析构），投递到 strand 串行处理。
        auto self = std::static_pointer_cast<impl>(shared_from_this());
        net::co_spawn(
            get_executor(),
            [self, sess = std::move(sess)]() mutable -> net::awaitable<void>
            {
                co_await self->async_release(std::move(sess));
                co_return;
            },
            net::detached);
    }

    net::awaitable<void>
    connection_pool::impl::async_release(std::unique_ptr<session> sess)
    {
        if (!sess)
        {
            co_return;
        }
        sess->set_query_logger({});
        auto self = shared_from_this();

        co_return co_await net::co_spawn(
            get_executor(),
            [&]() -> net::awaitable<void>
            {
                if (!is_running())
                {
                    co_return;
                }

                if (!sess->is_live())
                {
                    dec_active();
                    wake_one_waiter();
                    co_return;
                }

                if (sess->in_transaction())
                {
                    try
                    {
                        co_await sess->rollback();
                    }
                    catch (...)
                    {
                        dec_active();
                        wake_one_waiter();
                        co_return;
                    }
                }

                push_idle(std::move(sess));
            },
            net::use_awaitable);
    }

    void
    connection_pool::impl::stop()
    {
        // 同步翻转 is_running_（幂等）并取消维护循环，池立即拒绝新借出；
        // 同时把 teardown 投递到 strand，保证与后续 strand 任务按 FIFO 先于其执行
        // （on_stop 里还有一次兜底，teardown 幂等）。
        ticker::stop();

        auto self = std::static_pointer_cast<impl>(shared_from_this());
        net::dispatch(get_executor(), [self]() { self->teardown(); });
    }

    size_t
    connection_pool::impl::active_count() const
    {
        return active_metric_.load(std::memory_order_relaxed);
    }

    size_t
    connection_pool::impl::idle_count() const
    {
        return idle_metric_.load(std::memory_order_relaxed);
    }

    size_t
    connection_pool::impl::total_count() const
    {
        return active_metric_.load(std::memory_order_relaxed) + idle_metric_.load(std::memory_order_relaxed)
               + validating_metric_.load(std::memory_order_relaxed);
    }

    std::unique_ptr<session>
    connection_pool::impl::try_pop_idle()
    {
        while (!idle_.empty())
        {
            auto sess = std::move(idle_.back());
            idle_.pop_back();
            --idle_metric_;

            if (!sess)
            {
                continue;
            }
            if (!sess->is_live())
            {
                // 死连接移出 idle_ 后直接丢弃（strand 上析构，后端 close 不再持锁）。
                continue;
            }

            inc_active();
            return std::move(sess);
        }
        return nullptr;
    }

    net::awaitable<std::unique_ptr<session>>
    connection_pool::impl::acquire_or_create(bool serving)
    {
        // 未被唤醒的新请求不得越过已有等待者取连接/建连。
        if (!serving && has_live_waiter())
        {
            co_return nullptr;
        }

        while (true)
        {
            auto sess = try_pop_idle();
            if (!sess)
            {
                break;
            }

            if (cfg_.validate_on_borrow)
            {
                // 校验期间连接已移出 idle_，但仍占容量（active），避免超建。
                bool alive = false;
                try
                {
                    alive = co_await sess->ping();
                }
                catch (...)
                {
                    // 后端 ping 未承诺不抛：异常视同连接失效。
                    alive = false;
                }

                // stop() 后不能再把旧连接塞回池，也不能改计数（teardown 已清零）。
                if (!is_running())
                {
                    co_return nullptr;
                }

                if (!alive)
                {
                    dec_active();
                    get_logger()->warn("db pool: discarding dead idle connection");
                    wake_one_waiter(); // 校验剔除死连接释放了槽位，唤醒等待者接手
                    continue;
                }
            }

            co_return sess;
        }

        if (has_capacity())
        {
            inc_active();
            try
            {
                auto sess = co_await create_session();
                if (!sess)
                {
                    // 工厂返回空会话视为建连失败，释放槽位并唤醒下一个等待者。
                    throw db_exception(boost::system::errc::make_error_code(boost::system::errc::connection_aborted),
                                       "connection_pool: session factory returned an empty session");
                }
                co_return sess;
            }
            catch (...)
            {
                dec_active();
                // 本协程建连失败后槽位已释放，唤醒下一个等待者接手，避免其睡到超时。
                wake_one_waiter();
                throw;
            }
        }

        co_return nullptr;
    }

    void
    connection_pool::impl::wake_one_waiter()
    {
        while (!waiters_.empty())
        {
            auto w = waiters_.front();
            if (auto waiter = w.lock())
            {
                // 不弹出等待者：被唤醒的协程保留在队首，直到它借出、超时或被取消，
                // 新到的 acquire 会看到 live waiter 并排队，而不是插队。
                waiter->event.notify_one();
                return;
            }
            waiters_.pop_front();
        }
    }

    void
    connection_pool::impl::push_idle(std::unique_ptr<session> sess)
    {
        // stop() 后旧 handle 归还：直接丢弃会话，不改动计数（teardown 已清零）。
        if (!is_running())
        {
            return;
        }
        dec_active();
        if (sess)
        {
            sess->touch();
        }
        idle_.push_back(std::move(sess));
        ++idle_metric_;
        wake_one_waiter();
    }

    bool
    connection_pool::impl::has_live_waiter() noexcept
    {
        std::erase_if(waiters_, [](auto const& w) { return w.expired(); });
        return !waiters_.empty();
    }

    bool
    connection_pool::impl::can_serve() const noexcept
    {
        return !idle_.empty() || has_capacity();
    }

    void
    connection_pool::impl::remove_waiter(std::shared_ptr<waiter_node> const& node)
    {
        for (auto it = waiters_.begin(); it != waiters_.end();)
        {
            auto waiter = it->lock();
            if (!waiter)
            {
                it = waiters_.erase(it);
                continue;
            }
            if (waiter.get() == node.get())
            {
                it = waiters_.erase(it);
                break;
            }
            ++it;
        }

        // 队首 waiter 退出后，如果仍有空位/空闲连接，继续按 FIFO 唤醒下一个。
        if (has_live_waiter() && can_serve())
        {
            wake_one_waiter();
        }
    }

    net::awaitable<std::unique_ptr<session>>
    connection_pool::impl::create_session()
    {
        // 借出的会话也在池 strand 上运行，与 http_client_pool 借出的 client 共 executor 一致。
        co_return co_await connect_(base_executor_);
    }

    net::awaitable<void>
    connection_pool::impl::on_stop()
    {
        // ticker 的 run loop 退出后在本 strand 上执行：回收 idle 连接并唤醒等待者（幂等）。
        teardown();
        get_logger()->debug("db pool stopped");
        co_return;
    }

    void
    connection_pool::impl::teardown()
    {
        waiters_list pending_waiters;
        std::vector<std::unique_ptr<session>> to_close;

        pending_waiters.swap(waiters_);
        to_close = std::move(idle_);

        // 与 http_client_pool 对齐：停止即清零计数；旧 handle 归还时因 !is_running() 被丢弃。
        idle_metric_.store(0, std::memory_order_relaxed);
        active_metric_.store(0, std::memory_order_relaxed);
        validating_metric_.store(0, std::memory_order_relaxed);

        // 唤醒所有等待者（close → wait 返回 closed → 循环顶部看到 !is_running → 抛 pool_closed）。
        for (auto& w : pending_waiters)
        {
            if (auto waiter = w.lock())
            {
                waiter->event.close();
            }
        }
        // to_close 在函数返回时于锁外析构（session 析构可能进入后端 close）。
    }

    net::awaitable<bool>
    connection_pool::impl::on_tick()
    {
        auto now = std::chrono::steady_clock::now();

        // 维护协程运行在池 strand 上，池状态访问无需加锁。
        std::vector<std::unique_ptr<session>> to_ping;
        std::vector<std::unique_ptr<session>> to_close;
        {
            if (!is_running())
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
                        --idle_metric_;
                        wake_one_waiter(); // 空闲回收释放了槽位，唤醒等待者避免其睡到超时
                        continue;
                    }
                }

                if (cfg_.health_check_interval.count() > 0 && since_ping >= cfg_.health_check_interval)
                {
                    to_ping.push_back(std::move(*it));
                    it = idle_.erase(it);
                    --idle_metric_;
                    ++validating_metric_;
                }
                else
                {
                    ++it;
                }
            }
        }

        // 空闲回收的连接在 strand 上析构（后端 close 不再持锁）。
        to_close.clear();

        for (auto& sess : to_ping)
        {
            // stop() 后不再发 ping，但仍要走下面的归还/剔除记账，
            // 保证 validating_metric_ 无论何种退出路径都精确归零。
            bool alive = false;
            if (sess && is_running())
            {
                try
                {
                    alive = co_await sess->ping();
                }
                catch (...)
                {
                    // 后端 ping 未承诺不抛：异常视同连接失效。既保证 validating_metric_ 精确归零，
                    // 也不让单个坏连接中断整个维护协程。
                    alive = false;
                }
            }

            if (validating_metric_.load(std::memory_order_relaxed) > 0)
            {
                --validating_metric_;
            }
            // 健康检查期间池子可能已被新连接补满：超容量时直接关闭刚检查完的连接，
            // 避免总连接数短暂突破 max_connections 后迟迟不回收。
            if (alive && is_running() && has_capacity())
            {
                idle_.push_back(std::move(sess));
                ++idle_metric_;
                wake_one_waiter();
            }
            else if (is_running())
            {
                wake_one_waiter(); // 剔除死连接释放了槽位，唤醒等待者接手
            }
        }

        if (!is_running())
        {
            co_return false;
        }

        size_t deficit = 0;
        // 验证中的连接计入总数，否则健康检查窗口内会误判缺额而超建。
        if (size_t total = total_size(); total < cfg_.min_connections)
        {
            deficit = cfg_.min_connections - total;
        }

        for (size_t i = 0; i < deficit; ++i)
        {
            if (!is_running())
            {
                co_return false;
            }
            try
            {
                auto sess = co_await create_session();
                if (sess)
                {
                    // 建连期间 acquire 侧可能已把池补满：超容量时丢弃，避免总连接数突破 max_connections。
                    if (is_running() && has_capacity())
                    {
                        idle_.push_back(std::move(sess));
                        ++idle_metric_;
                        wake_one_waiter();
                    }
                }
            }
            catch (boost::system::system_error const& e)
            {
                // 维护协程被 stop() 取消时，挂起的建连以 operation_aborted 抛出；
                // 据此区分"调用方主动停机"与"真实建连失败"，仅在真实失败时告警。
                auto ec = e.code();
                if (ec != net::error::operation_aborted)
                {
                    get_logger()->warn("db pool refill connection failed: {}", ec.message());
                }
            }
            catch (std::exception const& ex)
            {
                get_logger()->warn("db pool refill connection failed: {}", ex.what());
            }
            catch (...)
            {
                get_logger()->warn("db pool refill connection failed: unknown error");
            }
        }

        co_return true;
    }

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
        impl_->start();
    }

    connection_pool::~connection_pool() { stop(); }

    connection_pool::connection_pool(connection_pool&&) noexcept = default;
    connection_pool& connection_pool::operator=(connection_pool&&) noexcept = default;

    net::awaitable<connection_pool::session_handle>
    connection_pool::async_acquire(std::chrono::steady_clock::duration wait_timeout)
    {
        co_return co_await impl_->async_acquire(wait_timeout);
    }

    void
    connection_pool::stop()
    {
        if (impl_)
        {
            impl_->stop();
        }
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
    connection_pool::get_executor() const noexcept
    {
        return impl_->get_executor();
    }

    std::shared_ptr<spdlog::logger>
    connection_pool::logger() const
    {
        return impl_->get_logger();
    }

    void
    connection_pool::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        impl_->set_logger(std::move(logger));
    }

    connection_pool
    make_pool(net::any_io_executor ex, std::string_view backend_name, std::string_view conn_string, pool_params cfg)
    {
        detail::register_backends();
        if (!detail::find_backend(backend_name))
        {
            throw db_exception(boost::system::error_code {},
                               "db: unknown backend '" + std::string(backend_name)
                                   + "' (registered: " + detail::registered_backend_names() + ")");
        }
        return connection_pool(
            ex,
            std::move(cfg),
            [name = std::string(backend_name),
             conn = std::string(conn_string)](net::any_io_executor pool_ex) -> net::awaitable<std::unique_ptr<session>>
            { co_return std::make_unique<session>(co_await session::connect(pool_ex, name, conn)); });
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
