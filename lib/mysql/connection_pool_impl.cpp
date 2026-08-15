#ifdef HTTPLIB_ENABLED_DATABASE
#include "mysql/connection_pool_impl.h"
#include "httplib/util/use_awaitable.hpp"
#include "mysql/session_impl.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace httplib::mysql
{

    connection_pool::impl::impl(net::any_io_executor ex, pool_params cfg)
        : ex_(ex)
        , cfg_(std::move(cfg))
        , maintain_timer_(ex)
    {
        if (cfg_.min_connections > cfg_.max_connections)
        {
            cfg_.min_connections = cfg_.max_connections;
        }

        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        spdlog::sinks_init_list sink_list = { console_sink };
        default_logger_ = std::make_shared<spdlog::logger>("httplib.mysql_pool", sink_list);
        default_logger_->set_level(spdlog::level::info);
    }

    connection_pool::impl::~impl()
    {
        stop();
    }

    std::shared_ptr<spdlog::logger>
    connection_pool::impl::logger() const
    {
        return custom_logger_ ? custom_logger_ : default_logger_;
    }

    void
    connection_pool::impl::set_logger(std::shared_ptr<spdlog::logger> l)
    {
        custom_logger_ = std::move(l);
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
                        self->logger()->error("mysql pool maintenance failed: {}", ex.what());
                    }
                    catch (...)
                    {
                        self->logger()->error("mysql pool maintenance failed: unknown error");
                    }
                }
            });
    }

    net::awaitable<connection_pool::session_handle>
    connection_pool::impl::async_acquire(std::chrono::steady_clock::duration wait_timeout)
    {
        if (stopped_)
        {
            throw std::runtime_error("connection_pool: pool is shut down");
        }

        // 0（默认）表示不设超时，一直等到有连接可用
        auto deadline = wait_timeout <= std::chrono::steady_clock::duration::zero()
                            ? std::chrono::steady_clock::time_point::max()
                            : std::chrono::steady_clock::now() + wait_timeout;

        auto self = shared_from_this();

        do
        {
            if (auto sess = co_await self->try_pop_validated())
            {
                co_return session_handle(self, std::move(sess));
            }

            std::unique_lock<std::mutex> lock(self->mutex_);
            if (self->stopped_)
            {
                throw std::runtime_error("connection_pool: pool is shut down");
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

            if (deadline <= std::chrono::steady_clock::now())
            {
                throw std::runtime_error("connection_pool: acquire timeout");
            }

            std::erase_if(self->waiters_, [](auto const& w) { return w.expired(); });

            auto node = std::make_shared<net::steady_timer>(self->ex_);
            if (deadline == std::chrono::steady_clock::time_point::max())
            {
                // 无超时：设一个很长的唤醒间隔兜底，正常靠 release 时 cancel 打断
                node->expires_after(std::chrono::hours(24));
            }
            else
            {
                node->expires_at(deadline);
            }
            self->waiters_.push_back(node);
            lock.unlock();

            boost::system::error_code ec;
            co_await node->async_wait(util::net_awaitable[ec]);
        } while (deadline > std::chrono::steady_clock::now());

        throw std::runtime_error("connection_pool: acquire timeout");
    }

    void
    connection_pool::impl::release_session(std::unique_ptr<session> sess)
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

        if (!imp.in_transaction)
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
    connection_pool::impl::stop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_.exchange(true))
        {
            return;
        }
        epoch_.fetch_add(1);
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
        return active_count_ + idle_.size();
    }

    std::unique_ptr<session>
    connection_pool::impl::try_pop_idle()
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

    net::awaitable<std::unique_ptr<session>>
    connection_pool::impl::try_pop_validated()
    {
        std::unique_ptr<session> sess;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sess = try_pop_idle();
        }
        if (!sess)
        {
            co_return nullptr;
        }

        if (!cfg_.validate_on_borrow || co_await sess->ping())
        {
            co_return sess;
        }

        // 连接已失效：丢弃，由外层重新创建新连接
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_count_ > 0)
        {
            --active_count_;
        }
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
            get_impl(*sess).touch();
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

    net::awaitable<std::unique_ptr<session>>
    connection_pool::impl::create_session()
    {
        co_return std::make_unique<session>(co_await session::connect(ex_, cfg_));
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
                logger()->warn("mysql pool pre-create connection failed: {}", ex.what());
            }
            catch (...)
            {
                logger()->warn("mysql pool pre-create connection failed: unknown error");
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_ || epoch_ != epoch)
            {
                co_return;
            }
            for (auto& sess : pre_created)
            {
                idle_.push_back(std::move(sess));
            }
        }

        co_await co_maintain(epoch);
        co_return;
    }

    net::awaitable<void>
    connection_pool::impl::co_maintain(uint64_t epoch)
    {
        auto check_interval
            = cfg_.idle_check_interval.count() > 0 ? cfg_.idle_check_interval : std::chrono::seconds(60);

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
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopped_)
                {
                    co_return;
                }

                for (auto it = idle_.begin(); it != idle_.end();)
                {
                    auto& imp = get_impl(**it);
                    auto idle_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - imp.last_active);
                    auto since_ping = std::chrono::duration_cast<std::chrono::seconds>(now - imp.last_ping);

                    if (cfg_.idle_timeout.count() > 0 && idle_elapsed >= cfg_.idle_timeout)
                    {
                        if (idle_.size() > cfg_.min_connections)
                        {
                            it = idle_.erase(it);
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
            }

            for (auto& sess : to_ping)
            {
                if (stopped_ || epoch_ != epoch)
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
                        if (!stopped_)
                        {
                            idle_.push_back(std::move(sess));
                        }
                    }
                }
                catch (std::exception const& ex)
                {
                    logger()->warn("mysql pool refill connection failed: {}", ex.what());
                }
                catch (...)
                {
                    logger()->warn("mysql pool refill connection failed: unknown error");
                }
            }
        }
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
