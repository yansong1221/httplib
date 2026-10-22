#include "httplib/client/client_pool.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/error.hpp"
#include "httplib/util/async_event.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/string_hash.hpp"
#include "httplib/util/ticker.hpp"
#include "util/logging.hpp"
#include "httplib/url/url.hpp"
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
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
            std::size_t active_count = 0;
            /// borrow 校验期间已移出 idle、但尚未决定保留/丢弃的连接数；占 route 容量。
            std::size_t validating_count = 0;
        };

        struct waiter_node
        {
            /// 被池唤醒（notify_one）或到达等待截止时间（wait_for 超时）。被唤醒者
            /// 保留在队首直到借出/超时/取消，保证先到先服务。
            util::async_event event;

            explicit waiter_node(net::any_io_executor const& ex) : event(ex) {}
        };

        /// acquire_or_create 的内部结果：把「暂无容量，请去排队」这个控制信号
        /// 从错误码词汇里彻底剥离。`wait == true` 表示暂无容量、调用方应走等待
        /// 路径，此时 `handle` 无意义；否则 `handle` 是一次成功借出。
        struct acquire_step
        {
            http_client_pool::client_handle handle;
            bool wait = false;
        };

    } // namespace

    class http_client_pool::impl
        : public detail::logger
        , public util::ticker
    {
      public:
        impl(net::any_io_executor const& ex, pool_params cfg)
            : detail::logger("httplib.client_pool")
            , ticker(net::make_strand(ex))
            , executor_(ex)
            , cfg_(std::move(cfg))
        {
            auto interval = cfg_.idle_check_interval.count() > 0 ? cfg_.idle_check_interval : std::chrono::seconds(60);
            set_interval(interval);
        }

        ~impl() {}

        net::awaitable<bool>
        on_start() override
        {
            get_logger()->debug("client pool started");
            co_return true;
        }
        net::awaitable<void>
        on_stop() override
        {
            // ticker 的 run loop 退出后在本 strand 上执行：回收 idle 连接并唤醒等待者。
            co_await async_teardown();
            get_logger()->debug("client pool stopped");
            co_return;
        }

        net::awaitable<client_handle>
        async_acquire(std::string_view host,
                      uint16_t port,
                      url::scheme s,
                      std::chrono::steady_clock::duration wait_timeout)
        {
            // 池状态只在自身 strand 上访问：把调用协程切到 strand 后再操作，
            // 后续 await（校验/等待唤醒）都会在 strand 上恢复，因此无需再加锁。
            co_return co_await net::co_spawn(
                get_executor(),
                [&]() -> net::awaitable<client_handle>
                {
                    if (!is_running())
                    {
                        co_return client_handle(
                            boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
                    }

                    auto url = url::make_url_value(host, port, s);

                    // wait_timeout <= 0 means "fail fast": try once and return an
                    // acquire_timeout immediately if no connection is available
                    // without waiting. The deadline is only consulted on the
                    // waiting path (wait_timeout > 0).
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
                                remove_waiter(url, node);
                            }
                            co_return client_handle(
                                boost::system::errc::make_error_code(boost::system::errc::operation_canceled));
                        }

                        if (in_queue && deadline <= std::chrono::steady_clock::now())
                        {
                            remove_waiter(url, node);
                            get_logger()->debug("client pool: acquire timed out for {}", url);
                            co_return client_handle(make_error_code(error::acquire_timeout));
                        }

                        // 被唤醒的等待者保留在队首；只有它自己能绕过“已有等待者”的公平性检查。
                        auto step = co_await acquire_or_create(url, host, port, s, serving);

                        // 借到连接就直接返回；否则（wait == true，暂无容量）落到等待/排队路径。
                        if (step.handle || !step.wait)
                        {
                            if (in_queue)
                            {
                                remove_waiter(url, node);
                            }
                            co_return std::move(step.handle);
                        }

                        serving = false;

                        if (wait_timeout <= std::chrono::steady_clock::duration::zero())
                        {
                            if (in_queue)
                            {
                                remove_waiter(url, node);
                            }
                            get_logger()->debug("client pool: no available connection for {} (fail fast)", url);
                            co_return client_handle(make_error_code(error::acquire_timeout));
                        }

                        auto& waiters = waiters_[url];
                        std::erase_if(waiters, [](auto const& w) { return w.expired(); });

                        if (!in_queue)
                        {
                            node = std::make_shared<waiter_node>(get_executor());
                            waiters.push_back(node);
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
        release(std::unique_ptr<http_client> conn)
        {
            // 归还可能来自任意线程（client_handle 析构），投递到 strand 串行处理。
            net::co_spawn(
                get_executor(),
                [this, self = shared_from_this(), conn = std::move(conn)]() mutable -> net::awaitable<void>
                {
                    co_await async_release(std::move(conn));
                    co_return;
                },
                net::detached);
        }

        net::awaitable<void>
        async_release(std::unique_ptr<http_client> conn)
        {
            co_await net::dispatch(get_executor(), net::use_awaitable);

            if (!is_running())
            {
                co_return;
            }
            auto url = url::make_url_value(conn->host(), conn->port(), conn->scheme());
            auto st_it = pools_.find(url);
            if (st_it == pools_.end())
            {
                co_return;
            }
            dec_active(st_it->second);

            if (!conn->has_active_session() && st_it->second.active_count < cfg_.max_size)
            {
                // 归池前重置为 pool 配置，避免上一个 borrower 的 timeout/redirect/SSL/logger/CA
                // 设置污染下一次借出。
                apply_client_settings(*conn);
                st_it->second.idle.push_back({ std::move(conn), std::chrono::steady_clock::now() });
                get_logger()->trace("client pool: returned connection to idle for {}", url);
            }
            else
            {
                track_destroyed();
                get_logger()->trace("client pool: closed connection for {}", url);
            }

            wake_one_waiter(url);
        }
        void
        stop() override
        {
            // 同步翻转 is_running_（幂等）并取消维护循环，池立即拒绝新借出；
            // 同时把 teardown 投递到 strand，保证与后续 strand 任务按 FIFO 先于其执行
            // （on_stop 里还有一次兜底，teardown 幂等）。
            ticker::stop();

            net::co_spawn(
                get_executor(),
                [this, self = shared_from_this()]() -> net::awaitable<void>
                {
                    co_await async_teardown();
                    co_return;
                },
                net::detached);
        }

        /// 释放所有 idle 连接并唤醒等待者；仅在 strand 上执行（或析构时独占执行）。
        net::awaitable<void>
        async_teardown()
        {
            co_await net::dispatch(get_executor(), net::use_awaitable);

            std::vector<waiters_list> pending_waiters;
            std::vector<std::unique_ptr<http_client>> to_close;

            pending_waiters.reserve(waiters_.size());
            for (auto& [url, waiters] : waiters_)
            {
                pending_waiters.push_back(std::move(waiters));
            }
            waiters_.clear();

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

            // close/析构 idle 连接（此时已不持有任何池状态）。
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
                        waiter->event.close();
                    }
                }
            }
        }

        size_t
        active_count() const
        {
            return total_active_.load();
        }

        size_t
        idle_count() const
        {
            return total_connections_.load() - total_active_.load();
        }

        size_t
        total_count() const
        {
            return total_connections_.load();
        }

        // ---- async 版本：先切到池执行器，再读取池状态，可从任意线程调用 ----

        net::awaitable<pool_stats>
        async_stats(std::string key) const
        {
            co_await net::dispatch(get_executor(), net::use_awaitable);

            pool_stats s;
            auto it = pools_.find(key);
            if (it != pools_.end())
            {
                s.idle = it->second.idle.size();
                s.active = it->second.active_count > 0 ? static_cast<size_t>(it->second.active_count) : 0;
            }
            co_return s;
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
            if (total_connections_.load() > 0)
            {
                --total_connections_;
            }
        }

        // ---- 计数/容量辅助（仅在 strand 上访问）----

        bool
        has_capacity(std::string const& url) const noexcept
        {
            bool route_ok = true;
            if (auto it = pools_.find(url); it != pools_.end())
            {
                route_ok
                    = it->second.active_count + it->second.idle.size() + it->second.validating_count < cfg_.max_size;
            }
            else if (cfg_.max_size == 0)
            {
                route_ok = false;
            }

            return route_ok && (cfg_.max_total == 0 || total_connections_.load() < cfg_.max_total);
        }

        void
        inc_active(pool_state& st) noexcept
        {
            ++st.active_count;
            ++total_active_;
        }

        void
        dec_active(pool_state& st) noexcept
        {
            if (st.active_count > 0 && total_active_.load() > 0)
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
            // 清掉上一个 borrower 设置的限速（例如下载器按连接分摊的速率），
            // 否则下一位借用者会继承不属于它的限速。0 表示不限速。
            c.set_download_rate_limit(0);
            c.set_upload_rate_limit(0);
            c.set_logger(get_logger());
        }

        bool
        has_live_waiter(std::string const& url) noexcept
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
        can_serve(std::string const& url) const noexcept
        {
            if (auto it = pools_.find(url); it != pools_.end() && !it->second.idle.empty())
            {
                return true;
            }
            return has_capacity(url);
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
                    waiter->event.notify_one();
                    return;
                }
                waiters.pop_front();
            }
            waiters_.erase(it);
        }

        void
        remove_waiter(std::string const& url, std::shared_ptr<waiter_node> const& node)
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
            if (has_live_waiter(url) && can_serve(url))
            {
                wake_one_waiter(url);
            }
        }

        void
        cleanup_waiters() noexcept
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

        /// 用当前池对象构造一个成功借出的 handle。
        client_handle
        make_borrowed_handle(std::unique_ptr<http_client> conn)
        {
            return client_handle(std::static_pointer_cast<http_client_pool::impl>(shared_from_this()), std::move(conn));
        }

        /// 新建一条连接并借出。容量检查由调用方负责；本函数必然成功返回一个 handle。
        client_handle
        create_connection(std::string const& url, std::string_view host, uint16_t port, url::scheme s)
        {
            auto& st = pools_[url];
            inc_active(st);
            track_created();
            // 用 host/port/scheme 直接构造。不要传字符串 URL：那个构造函数会再
            // parse 一遍，而 host 来自调用方、未必是合法的 URL 片段（可能含空格等
            // 未转义字符），二次 parse 失败会抛异常，把本就该延迟到 connect 才暴露的
            // 问题提前变成建连失败。
            // 传底层 executor（非池 strand）：client 各自 make_strand，连接之间互不串行。
            auto cli = std::make_unique<http_client>(executor_, host, port, s);
            apply_client_settings(*cli);
            get_logger()->debug("client pool: created connection for {} (total={})",
                                url,
                                total_connections_.load(std::memory_order_relaxed));
            return make_borrowed_handle(std::move(cli));
        }

        net::awaitable<acquire_step>
        acquire_or_create(std::string const& url,
                          std::string_view host,
                          uint16_t port,
                          url::scheme s,
                          bool serving)
        {
            // 未被唤醒的新请求不得越过已有等待者取连接/建连。
            if (!serving && has_live_waiter(url))
            {
                co_return acquire_step { {}, true };
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

                    bool alive = false;
                    try
                    {
                        alive = co_await conn->async_is_alive();
                    }
                    catch (...)
                    {
                        alive = false;
                    }

                    // stop() 后不能再把旧连接塞回池，也不能改池计数。
                    if (!is_running())
                    {
                        co_return acquire_step { {}, true };
                    }

                    it = pools_.find(url);
                    if (it == pools_.end())
                    {
                        co_return acquire_step { {}, true };
                    }

                    auto& validated_st = it->second;
                    if (validated_st.validating_count > 0)
                    {
                        --validated_st.validating_count;
                    }

                    if (!alive)
                    {
                        track_destroyed();
                        get_logger()->warn("client pool: discarding dead idle connection for {}", url);
                        wake_one_waiter(url); // 死连接释放了容量，唤醒等待者接手
                        continue;
                    }

                    inc_active(validated_st);
                    co_return acquire_step { make_borrowed_handle(std::move(conn)), false };
                }

                inc_active(st);
                co_return acquire_step { make_borrowed_handle(std::move(conn)), false };
            }

            // 没有可复用的空闲连接：有容量就新建，否则交回等待路径。
            // 只有真去新建时才触碰 pools_，避免一次空转的借用留下空的 pool_state 条目。
            if (has_capacity(url))
            {
                co_return acquire_step { create_connection(url, host, port, s), false };
            }

            co_return acquire_step { {}, true };
        }

        net::awaitable<bool>
        on_tick() override
        {
            auto now = std::chrono::steady_clock::now();
            std::vector<std::unique_ptr<http_client>> to_close;
            {
                cleanup_waiters();
                for (auto it = pools_.begin(); it != pools_.end();)
                {
                    auto& st = it->second;
                    auto it2 = st.idle.begin();
                    while (it2 != st.idle.end())
                    {
                        if (cfg_.idle_timeout.count() > 0 && now - it2->idle_since > cfg_.idle_timeout)
                        {
                            get_logger()->trace("client pool: evicting idle connection for {}", it->first);
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
            co_return true;
        }

      private:
        /// 底层 executor（非 strand）：client 用它各自 make_strand，得到独立 strand，
        /// 避免所有 client 嵌套在池 strand 上被串行化。池自身状态仍用 ticker 的 strand。
        net::any_io_executor executor_;
        using waiters_list = std::deque<std::weak_ptr<waiter_node>>;

        util::string_map<pool_state> pools_;
        std::atomic<size_t> total_connections_ { 0 };
        std::atomic<size_t> total_active_ { 0 };
        pool_params cfg_;

        util::string_map<waiters_list> waiters_;
    };

    // ---- client_handle ----

    http_client_pool::client_handle::client_handle()
        : client_handle(make_error_code(error::invalid_handle))
    {
    }

    http_client_pool::client_handle::client_handle(std::weak_ptr<impl> pool, std::unique_ptr<http_client> conn)
        : pool_(std::move(pool))
        , conn_(std::move(conn))
    {
    }

    http_client_pool::client_handle::client_handle(boost::system::error_code ec) : error_(ec) {}

    http_client_pool::client_handle::client_handle(client_handle&& other) noexcept
        : pool_(std::move(other.pool_))
        , conn_(std::move(other.conn_))
        , error_(other.error_)
    {
        // 成功 handle 被 move 后源对象变为 expired；失败 handle 保留原错误码。
        if (!other.error_ && other.conn_ == nullptr)
        {
            other.error_ = make_error_code(error::invalid_handle);
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
            if (other_was_live && !other.error_)
            {
                other.error_ = make_error_code(error::invalid_handle);
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
            pool->release(std::move(conn_));
            if (!error_)
            {
                error_ = make_error_code(error::invalid_handle);
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
        impl_->start();
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

    net::awaitable<http_client_pool::client_handle>
    http_client_pool::async_acquire(std::string_view host,
                                    uint16_t port,
                                    url::scheme s /*= url::scheme::plain*/,
                                    std::chrono::steady_clock::duration wait_timeout /*= default_timeout*/)
    {
        co_return co_await impl_->async_acquire(host, port, s, wait_timeout);
    }

    net::awaitable<http_client_pool::client_handle>
    http_client_pool::async_acquire(std::string_view url,
                                    std::chrono::steady_clock::duration wait_timeout /*= default_timeout*/)
    {
        auto r = url::parse_url(url);
        if (!r)
        {
            // 与 downloader 保持一致：URL 解析失败统一映射为模块错误码，
            // 而不是把 url 模块的原始错误码漏给调用方。
            co_return client_handle(make_error_code(error::invalid_url));
        }
        auto const& u = *r;
        co_return co_await impl_->async_acquire(u.host, u.effective_port(), u.transport(), wait_timeout);
    }

    std::shared_ptr<spdlog::logger>
    http_client_pool::logger() const
    {
        return impl_->get_logger();
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
        // 同步翻转 is_running_（ticker::stop 幂等）并取消维护循环；实际清理在
        // ticker 的 on_stop() 于池 strand 上执行，无需在此等待。
        impl_->stop();
    }

    std::future<http_client_pool::pool_stats>
    http_client_pool::stats(std::string_view host, uint16_t port, url::scheme s /*= url::scheme::plain*/) const
    {
        auto impl = impl_;
        return net::co_spawn(
            impl->get_executor(),
            [impl, key = url::make_url_value(host, port, s)]() mutable -> net::awaitable<pool_stats>
            { co_return co_await impl->async_stats(std::move(key)); },
            net::use_future);
    }

    std::future<http_client_pool::pool_stats>
    http_client_pool::stats(std::string_view url) const
    {
        auto impl = impl_;
        return net::co_spawn(
            impl->get_executor(),
            [impl, raw = std::string(url)]() -> net::awaitable<pool_stats>
            {
                auto parsed = url::parse_url(raw);
                if (!parsed)
                {
                    co_return pool_stats {};
                }
                co_return co_await impl->async_stats(
                    url::make_url_value(parsed->host, parsed->effective_port(), parsed->transport()));
            },
            net::use_future);
    }

    net::awaitable<http_client_pool::pool_stats>
    http_client_pool::async_stats(std::string_view host, uint16_t port, url::scheme s /*= url::scheme::plain*/) const
    {
        auto impl = impl_;
        co_return co_await impl->async_stats(url::make_url_value(host, port, s));
    }

    net::awaitable<http_client_pool::pool_stats>
    http_client_pool::async_stats(std::string_view url) const
    {
        auto impl = impl_;
        auto r = url::parse_url(url);
        if (!r)
        {
            co_return pool_stats {};
        }
        auto const& u = *r;
        co_return co_await impl->async_stats(url::make_url_value(u.host, u.effective_port(), u.transport()));
    }

} // namespace httplib::client
