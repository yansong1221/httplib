#include "httplib/client/client_pool.hpp"
#include "httplib/client/client.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "util/logging.hpp"
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/system_error.hpp>
#include <boost/url.hpp>
#include <deque>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace httplib::client
{

    namespace
    {

        struct pooled_conn
        {
            std::unique_ptr<http_client> client;
            std::chrono::steady_clock::time_point idle_since;
        };

        struct pool_state
        {
            std::deque<pooled_conn> idle;
            int64_t active_count = 0;
            /// borrow 校验期间已移出 idle、但尚未决定保留/丢弃的连接数；占 route 容量。
            int64_t validating_count = 0;
        };

        struct waiter_node
        {
            net::steady_timer timer;
            /// 被池唤醒后保持等待者留在队首，直到它借出/超时/取消，保证先到先服务。
            std::atomic<bool> woken { false };

            explicit waiter_node(net::any_io_executor const& ex) : timer(ex) {}
        };

    } // namespace

    class http_client_pool::impl : public std::enable_shared_from_this<impl>
    {
      public:
        impl(net::any_io_executor const& ex, pool_params cfg) : ex_(ex), cfg_(std::move(cfg))
        {
            default_logger_ = httplib::detail::make_console_logger("httplib.client_pool");
        }

        ~impl() { stop(); }

        std::shared_ptr<spdlog::logger>
        logger() const
        {
            auto l = custom_logger_.load();
            return l ? l : default_logger_;
        }

        void
        set_logger(std::shared_ptr<spdlog::logger> l)
        {
            custom_logger_.store(std::move(l));
        }

        void
        start()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopped_.exchange(false))
            {
                return;
            }
            logger()->debug("client pool started");

            // Each maintenance loop owns a fresh timer. stop() cancels the timer it
            // finds here, so a stop()/start() sequence can never make two co_maintain
            // coroutines share a single steady_timer (which would be UB).
            cleanup_timer_ = std::make_shared<net::steady_timer>(ex_);
            auto timer = cleanup_timer_;

            net::co_spawn(
                ex_,
                [this, self = shared_from_this(), timer]() -> net::awaitable<void> { co_await co_maintain(timer); },
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
                            self->logger()->error("client pool maintenance failed: {}", ex.what());
                        }
                        catch (...)
                        {
                            self->logger()->error("client pool maintenance failed: unknown error");
                        }
                    }
                });
        }

        net::awaitable<client_handle>
        async_acquire(std::string_view host, uint16_t port, bool ssl, std::chrono::steady_clock::duration wait_timeout)
        {
            if (stopped_)
            {
                co_return client_handle(boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
            }

            auto self = shared_from_this();
            auto url = util::make_url_value(host, port, ssl);

            // wait_timeout <= 0 means "fail fast": try once and return timed_out
            // immediately if no connection is available without waiting. The deadline
            // is only consulted on the waiting path (wait_timeout > 0).
            auto deadline = std::chrono::steady_clock::now() + wait_timeout;

            std::shared_ptr<waiter_node> node;
            bool in_queue = false;

            do
            {
                std::unique_lock<std::mutex> lock(self->mutex_);
                if (self->stopped_)
                {
                    if (in_queue)
                    {
                        self->remove_waiter_locked(url, node);
                    }
                    co_return client_handle(
                        boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
                }

                if (in_queue && deadline <= std::chrono::steady_clock::now())
                {
                    self->remove_waiter_locked(url, node);
                    logger()->debug("client pool: acquire timed out for {}", url);
                    co_return client_handle(boost::system::errc::make_error_code(boost::system::errc::timed_out));
                }

                // 被唤醒的等待者保留在队首；只有它自己能绕过“已有等待者”的公平性检查。
                bool serving = in_queue && node->woken.load(std::memory_order_acquire);
                client_handle handle;
                try
                {
                    handle = self->acquire_or_create(url, serving);
                }
                catch (...)
                {
                    // 建连/配置路径抛异常时，必须先把当前 waiter 的回合交还队列，
                    // 否则后续 waiter 可能因队首失效而迟迟不被唤醒。
                    if (in_queue)
                    {
                        self->remove_waiter_locked(url, node);
                    }
                    throw;
                }

                if (handle)
                {
                    if (in_queue)
                    {
                        self->remove_waiter_locked(url, node);
                    }
                    co_return std::move(handle);
                }

                if (serving)
                {
                    node->woken.store(false, std::memory_order_release);
                }

                if (wait_timeout <= std::chrono::steady_clock::duration::zero())
                {
                    if (in_queue)
                    {
                        self->remove_waiter_locked(url, node);
                    }
                    logger()->debug("client pool: no available connection for {} (fail fast)", url);
                    co_return client_handle(boost::system::errc::make_error_code(boost::system::errc::timed_out));
                }

                auto& waiters = self->waiters_[url];
                std::erase_if(waiters, [](auto const& w) { return w.expired(); });

                if (!in_queue)
                {
                    node = std::make_shared<waiter_node>(self->ex_);
                    waiters.push_back(node);
                    in_queue = true;
                }
                node->timer.expires_at(deadline);
                lock.unlock();

                boost::system::error_code ec;
                co_await node->timer.async_wait(util::net_awaitable[ec]);

            } while (true);
        }

        void
        release(std::unique_ptr<http_client> conn, uint64_t epoch)
        {
            auto url = util::make_url_value(conn->host(), conn->port(), conn->is_use_ssl());

            std::lock_guard<std::mutex> lock(mutex_);

            if (stopped_ || epoch != epoch_)
            {
                return;
            }

            auto st_it = pools_.find(url);
            if (st_it == pools_.end())
            {
                return;
            }
            dec_active_locked(st_it->second);

            if (!conn->has_active_session() && st_it->second.active_count < static_cast<int64_t>(cfg_.max_size))
            {
                // 归池前重置为 pool 配置，避免上一个 borrower 的 timeout/redirect/SSL/logger/CA 设置污染下一次借出。
                apply_client_settings(*conn);
                st_it->second.idle.push_back({ std::move(conn), std::chrono::steady_clock::now() });
                logger()->trace("client pool: returned connection to idle for {}", url);
            }
            else
            {
                track_destroyed();
                logger()->trace("client pool: closed connection for {}", url);
            }

            wake_one_waiter(url);
        }

        void
        stop()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopped_.exchange(true))
            {
                return;
            }
            // 递增 epoch：stop 前借出的 handle 在 restart 后归还时不得污染新池计数。
            ++epoch_;
            logger()->debug("client pool stopped");
            if (cleanup_timer_)
            {
                cleanup_timer_->cancel();
                cleanup_timer_.reset();
            }

            std::vector<waiters_list> pending_waiters;
            pending_waiters.reserve(waiters_.size());
            for (auto& [url, waiters] : waiters_)
            {
                pending_waiters.push_back(std::move(waiters));
            }
            waiters_.clear();

            std::vector<std::unique_ptr<http_client>> to_close;
            for (auto& [info, st] : pools_)
            {
                for (auto& pc : st.idle)
                {
                    to_close.push_back(std::move(pc.client));
                }
                st.idle.clear();
            }
            pools_.clear();
            total_connections_ = 0;
            total_active_ = 0;

            lock.unlock();

            // 锁外 close/析构 idle 连接。
            for (auto& conn : to_close)
            {
                conn->close();
            }

            for (auto& waiters : pending_waiters)
            {
                while (!waiters.empty())
                {
                    auto w = std::move(waiters.front());
                    waiters.pop_front();
                    if (auto waiter = w.lock(); waiter)
                    {
                        waiter->timer.cancel();
                    }
                }
            }
        }

        pool_stats
        stats(std::string const& url) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pool_stats s;
            auto it = pools_.find(url);
            if (it != pools_.end())
            {
                s.idle = it->second.idle.size();
                s.active = it->second.active_count > 0 ? static_cast<size_t>(it->second.active_count) : 0;
            }
            return s;
        }

        size_t
        active_count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return total_active_;
        }

        size_t
        idle_count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return total_connections_ - total_active_;
        }

        size_t
        total_count() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return total_connections_;
        }

      private:
        void
        track_created()
        {
            ++total_connections_;
        }

        void
        track_destroyed()
        {
            if (total_connections_ > 0)
            {
                --total_connections_;
            }
        }

        // ---- 持锁计数/容量辅助（调用前必须已持有 mutex_）----

        bool
        has_capacity_locked(std::string const& url) const noexcept
        {
            bool route_ok = true;
            if (auto it = pools_.find(url); it != pools_.end())
            {
                route_ok = it->second.active_count + static_cast<int64_t>(it->second.idle.size())
                               + it->second.validating_count
                           < static_cast<int64_t>(cfg_.max_size);
            }
            else if (cfg_.max_size == 0)
            {
                route_ok = false;
            }

            return route_ok && (cfg_.max_total == 0 || total_connections_ < cfg_.max_total);
        }

        void
        inc_active_locked(pool_state& st) noexcept
        {
            ++st.active_count;
            ++total_active_;
        }

        void
        dec_active_locked(pool_state& st) noexcept
        {
            if (st.active_count > 0 && total_active_ > 0)
            {
                --st.active_count;
                --total_active_;
            }
        }

        void
        apply_client_settings(http_client& c) const
        {
            c.set_timeout_policy(cfg_.timeout_policy);
            c.set_timeout(cfg_.timeout);
            c.set_max_redirects(cfg_.max_redirects);
            c.set_verify_ssl(cfg_.verify_ssl);
            // 复用连接必须清掉上一个 borrower 可能设置的 CA cert；空字符串表示回退系统默认。
            c.set_ca_cert(cfg_.ca_cert);
            c.set_logger(logger());
        }

        bool
        has_live_waiter_locked(std::string const& url) noexcept
        {
            auto it = waiters_.find(url);
            if (it == waiters_.end())
            {
                return false;
            }
            std::erase_if(it->second, [](auto const& w) { return w.expired(); });
            if (it->second.empty())
            {
                waiters_.erase(it);
                return false;
            }
            return true;
        }

        bool
        can_serve_locked(std::string const& url) const noexcept
        {
            if (auto it = pools_.find(url); it != pools_.end() && !it->second.idle.empty())
            {
                return true;
            }
            return has_capacity_locked(url);
        }

        void
        wake_one_waiter(std::string const& url)
        {
            auto it = waiters_.find(url);
            if (it == waiters_.end())
            {
                return;
            }
            auto& waiters = it->second;
            while (!waiters.empty())
            {
                auto w = waiters.front();
                if (auto waiter = w.lock())
                {
                    // 不弹出等待者：被唤醒的协程保留在队首，直到它借出、超时或被取消，
                    // 新到的 acquire 会看到 live waiter 并排队，而不是插队。
                    waiter->woken.store(true, std::memory_order_release);
                    waiter->timer.cancel();
                    return;
                }
                waiters.pop_front();
            }
            waiters_.erase(it);
        }

        void
        remove_waiter_locked(std::string const& url, std::shared_ptr<waiter_node> const& node)
        {
            auto it = waiters_.find(url);
            if (it == waiters_.end())
            {
                return;
            }
            auto& waiters = it->second;
            for (auto w_it = waiters.begin(); w_it != waiters.end();)
            {
                auto waiter = w_it->lock();
                if (!waiter)
                {
                    w_it = waiters.erase(w_it);
                    continue;
                }
                if (waiter.get() == node.get())
                {
                    w_it = waiters.erase(w_it);
                    break;
                }
                ++w_it;
            }
            if (waiters.empty())
            {
                waiters_.erase(it);
            }

            // 队首 waiter 退出后，如果仍有空位/空闲连接，继续按 FIFO 唤醒下一个。
            if (has_live_waiter_locked(url) && can_serve_locked(url))
            {
                wake_one_waiter(url);
            }
        }

        void
        cleanup_waiters_locked() noexcept
        {
            for (auto it = waiters_.begin(); it != waiters_.end();)
            {
                std::erase_if(it->second, [](auto const& w) { return w.expired(); });
                if (it->second.empty())
                {
                    it = waiters_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        client_handle
        acquire_or_create(std::string const& url, bool serving)
        {
            // 未被唤醒的新请求不得越过已有等待者取连接/建连。
            if (!serving && has_live_waiter_locked(url))
            {
                return {};
            }

            while (true)
            {
                auto it = pools_.find(url);
                if (it == pools_.end() || it->second.idle.empty())
                {
                    break;
                }

                auto& st = it->second;
                auto conn = std::move(st.idle.front().client);
                st.idle.pop_front();

                if (cfg_.validate_on_borrow)
                {
                    // 校验期间连接不在 idle 里，但必须继续占 route 容量。
                    ++st.validating_count;
                    auto epoch_at_check = epoch_;

                    bool alive = false;
                    try
                    {
                        alive = conn->is_alive();
                    }
                    catch (...)
                    {
                        alive = false;
                    }

                    // stop/restart 后不能再把旧连接塞回新池，也不能改新池计数。
                    if (stopped_ || epoch_at_check != epoch_)
                    {
                        return {};
                    }

                    it = pools_.find(url);
                    if (it == pools_.end())
                    {
                        return {};
                    }

                    auto& validated_st = it->second;
                    if (validated_st.validating_count > 0)
                    {
                        --validated_st.validating_count;
                    }

                    if (!alive)
                    {
                        track_destroyed();
                        logger()->warn("client pool: discarding dead idle connection for {}", url);
                        wake_one_waiter(url); // 死连接释放了容量，唤醒等待者接手
                        continue;
                    }

                    inc_active_locked(validated_st);
                    return client_handle(shared_from_this(), std::move(conn), epoch_);
                }

                inc_active_locked(st);
                return client_handle(shared_from_this(), std::move(conn), epoch_);
            }

            // Only touch pools_ when actually creating a connection, so a failed
            // acquire does not leave an empty pool_state entry behind.
            if (has_capacity_locked(url))
            {
                auto& st = pools_[url];
                inc_active_locked(st);
                track_created();
                try
                {
                    // 用 host/port/ssl 构造，避免 URL 二次 parse 失败把异常抛进 acquire 路径。
                    auto client = std::make_unique<http_client>(ex_, url);
                    apply_client_settings(*client);
                    logger()->debug("client pool: created connection for {} (total={})", url, total_connections_);
                    return client_handle(shared_from_this(), std::move(client), epoch_);
                }
                catch (...)
                {
                    dec_active_locked(st);
                    track_destroyed();
                    wake_one_waiter(url);
                    throw;
                }
            }

            return {};
        }

        net::awaitable<void>
        co_maintain(std::shared_ptr<net::steady_timer> timer)
        {
            boost::system::error_code ec;
            while (!stopped_)
            {
                auto interval
                    = cfg_.idle_check_interval.count() > 0 ? cfg_.idle_check_interval : std::chrono::seconds(60);

                timer->expires_after(interval);
                co_await timer->async_wait(util::net_awaitable[ec]);
                if (ec || stopped_)
                {
                    co_return;
                }

                auto now = std::chrono::steady_clock::now();
                std::vector<std::unique_ptr<http_client>> to_close;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stopped_)
                    {
                        co_return;
                    }
                    cleanup_waiters_locked();
                    for (auto it = pools_.begin(); it != pools_.end();)
                    {
                        auto& st = it->second;
                        auto it2 = st.idle.begin();
                        while (it2 != st.idle.end())
                        {
                            if (cfg_.idle_timeout.count() > 0 && now - it2->idle_since > cfg_.idle_timeout)
                            {
                                logger()->trace("client pool: evicting idle connection for {}", it->first);
                                to_close.push_back(std::move(it2->client));
                                it2 = st.idle.erase(it2);
                                track_destroyed();
                                wake_one_waiter(it->first); // 空闲回收释放了容量，唤醒等待者避免其睡到超时
                            }
                            else
                            {
                                ++it2;
                            }
                        }
                        if (st.idle.empty() && st.active_count == 0 && st.validating_count == 0)
                        {
                            it = pools_.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }

                // 锁外 close/析构，避免池锁内进入 http_client 内部锁。
                for (auto& conn : to_close)
                {
                    conn->close();
                }
            }
        }

      private:
        using waiters_list = std::deque<std::weak_ptr<waiter_node>>;

        mutable std::mutex mutex_;
        std::unordered_map<std::string, pool_state> pools_;
        size_t total_connections_ = 0;
        size_t total_active_ = 0;
        /// 受 mutex_ 保护；stop() 递增，使旧 handle 在 restart 后归还时失效。
        uint64_t epoch_ = 0;
        pool_params cfg_;

        std::shared_ptr<spdlog::logger> default_logger_;
        std::atomic<std::shared_ptr<spdlog::logger>> custom_logger_;

        std::unordered_map<std::string, waiters_list> waiters_;

      public:
        net::any_io_executor ex_;

      private:
        std::atomic<bool> stopped_ { true };
        std::shared_ptr<net::steady_timer> cleanup_timer_;
    };

    // ---- client_handle ----

    http_client_pool::client_handle::client_handle()
        : client_handle(boost::system::errc::make_error_code(boost::system::errc::not_connected))
    {
    }

    http_client_pool::client_handle::client_handle(std::weak_ptr<impl> pool,
                                                   std::unique_ptr<http_client> conn,
                                                   uint64_t epoch)
        : pool_(std::move(pool))
        , conn_(std::move(conn))
        , epoch_(epoch)
    {
    }

    http_client_pool::client_handle::client_handle(boost::system::error_code ec) : error_(ec) {}

    http_client_pool::client_handle::client_handle(client_handle&& other) noexcept
        : pool_(std::move(other.pool_))
        , conn_(std::move(other.conn_))
        , error_(other.error_)
        , epoch_(other.epoch_)
    {
        // 成功 handle 被 move 后源对象变为 expired；失败 handle 保留原错误码。
        if (!other.error_ && other.conn_ == nullptr)
        {
            other.error_ = boost::system::errc::make_error_code(boost::system::errc::not_connected);
        }
    }

    http_client_pool::client_handle&
    http_client_pool::client_handle::operator=(client_handle&& other) noexcept
    {
        if (this != &other)
        {
            release();
            bool other_was_live = other.conn_ != nullptr;
            pool_ = std::move(other.pool_);
            conn_ = std::move(other.conn_);
            error_ = other.error_;
            epoch_ = other.epoch_;
            if (other_was_live && !other.error_)
            {
                other.error_ = boost::system::errc::make_error_code(boost::system::errc::not_connected);
            }
        }
        return *this;
    }

    http_client_pool::client_handle::~client_handle() { release(); }

    void
    http_client_pool::client_handle::release()
    {
        auto pool = pool_.lock();
        if (pool && conn_)
        {
            pool->release(std::move(conn_), epoch_);
            if (!error_)
            {
                error_ = boost::system::errc::make_error_code(boost::system::errc::not_connected);
            }
        }
    }

    http_client*
    http_client_pool::client_handle::get()
    {
        if (has_error())
        {
            throw boost::system::system_error(error_);
        }
        return conn_.get();
    }

    http_client const*
    http_client_pool::client_handle::get() const
    {
        if (has_error())
        {
            throw boost::system::system_error(error_);
        }
        return conn_.get();
    }

    http_client_pool::client_handle::operator bool() const noexcept { return !has_error(); }

    bool
    http_client_pool::client_handle::has_error() const noexcept
    {
        return conn_ == nullptr;
    }

    http_client*
    http_client_pool::client_handle::operator->()
    {
        return get();
    }

    http_client const*
    http_client_pool::client_handle::operator->() const
    {
        return get();
    }

    http_client&
    http_client_pool::client_handle::operator*()
    {
        return *get();
    }

    http_client const&
    http_client_pool::client_handle::operator*() const
    {
        return *get();
    }

    boost::system::error_code const&
    http_client_pool::client_handle::error() const noexcept
    {
        return error_;
    }

    // ---- connection_pool ----

    http_client_pool::http_client_pool(net::any_io_executor const& ex, pool_params params)
        : impl_(std::make_shared<impl>(ex, params))
    {
    }

    http_client_pool::~http_client_pool()
    {
        if (impl_)
        {
            impl_->stop();
        }
    }

    http_client_pool::http_client_pool(http_client_pool&& other) noexcept = default;

    http_client_pool& http_client_pool::operator=(http_client_pool&& other) noexcept = default;

    void
    http_client_pool::start()
    {
        impl_->start();
    }

    net::awaitable<http_client_pool::client_handle>
    http_client_pool::async_acquire(std::string_view host,
                                    uint16_t port,
                                    bool ssl /*= false*/,
                                    std::chrono::steady_clock::duration wait_timeout /*= default_timeout*/)
    {
        co_return co_await impl_->async_acquire(host, port, ssl, wait_timeout);
    }

    net::awaitable<http_client_pool::client_handle>
    http_client_pool::async_acquire(std::string_view url,
                                    std::chrono::steady_clock::duration wait_timeout /*= default_timeout*/)
    {
        auto r = boost::urls::parse_uri(url);
        if (!r)
        {
            co_return client_handle(r.error());
        }
        auto const& u = *r;
        auto host = u.host();
        auto port = u.port_number() ? u.port_number() : (u.scheme_id() == boost::urls::scheme::https ? 443 : 80);
        auto ssl = u.scheme_id() == boost::urls::scheme::https;
        co_return co_await impl_->async_acquire(host, port, ssl, wait_timeout);
    }

    net::any_io_executor
    http_client_pool::get_executor() noexcept
    {
        return impl_->ex_;
    }

    std::shared_ptr<spdlog::logger>
    http_client_pool::logger() const
    {
        return impl_->logger();
    }

    void
    http_client_pool::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        impl_->set_logger(std::move(logger));
    }

    size_t
    http_client_pool::active_count() const
    {
        return impl_->active_count();
    }

    size_t
    http_client_pool::idle_count() const
    {
        return impl_->idle_count();
    }

    size_t
    http_client_pool::total_count() const
    {
        return impl_->total_count();
    }

    void
    http_client_pool::stop()
    {
        impl_->stop();
    }

    http_client_pool::pool_stats
    http_client_pool::stats(std::string_view host, uint16_t port, bool ssl /*= false*/) const
    {
        auto url = util::make_url_value(host, port, ssl);
        return impl_->stats(url);
    }

    httplib::client::http_client_pool::pool_stats
    http_client_pool::stats(std::string_view url) const
    {
        auto r = boost::urls::parse_uri(url);
        if (!r)
        {
            return {};
        }
        auto const& u = *r;
        auto host = u.host();
        auto port = u.port_number() ? u.port_number() : (u.scheme_id() == boost::urls::scheme::https ? 443 : 80);
        auto ssl = u.scheme_id() == boost::urls::scheme::https;
        return impl_->stats(util::make_url_value(host, port, ssl));
    }

} // namespace httplib::client
