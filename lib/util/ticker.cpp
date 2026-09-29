#include "httplib/util/ticker.hpp"
#include "httplib/util/sleep.hpp"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>

namespace httplib::util
{
    /// \ref ticker 的状态数据。公共头只暴露一个 `unique_ptr<impl>`，于是
    /// `net::cancellation_signal`、`std::mutex`、原子计数这些 Asio/线程设施
    /// 全部留在实现侧；run loop 的逻辑仍在 \ref ticker 自己的成员函数里。
    class ticker::impl
    {
      public:
        impl(net::any_io_executor const& executor, std::chrono::steady_clock::duration const& interval)
            : executor_(executor)
            , interval_(interval)
        {
        }

        net::any_io_executor executor_;

        std::atomic<std::chrono::steady_clock::duration> interval_;
        std::atomic<bool> is_running_ { false };
        std::atomic<uint64_t> run_id_ { 0 };

        std::mutex state_mutex_;
        std::shared_ptr<net::cancellation_signal> cs_;
    };

    ticker::ticker(net::any_io_executor const& executor, std::chrono::steady_clock::duration const& interval)
        : impl_(std::make_unique<impl>(executor, interval))
    {
    }

    ticker::~ticker() {}

    void
    ticker::start()
    {
        std::shared_ptr<net::cancellation_signal> cs;
        uint64_t generation = 0;

        {
            std::lock_guard<std::mutex> lock(impl_->state_mutex_);
            if (impl_->is_running_.load(std::memory_order_acquire))
            {
                return;
            }

            generation = ++impl_->run_id_;
            cs = std::make_shared<net::cancellation_signal>();
            impl_->cs_ = cs;
            impl_->is_running_.store(true, std::memory_order_release);

            net::co_spawn(
                impl_->executor_,
                [this, cs, generation, self = shared_from_this()]() -> net::awaitable<void> {
                    // 协程帧持有 self：run loop 期间派生类始终存活，
                    // on_start / on_tick / on_stop 也因此在派生类上解析。
                    co_await co_run();

                    if (impl_->run_id_.load(std::memory_order_acquire) == generation) {
                        impl_->is_running_.store(false, std::memory_order_release);
                    }
                },
                net::bind_cancellation_slot(cs->slot(), net::detached));
        }
    }

    void
    ticker::stop()
    {
        std::shared_ptr<net::cancellation_signal> cs;

        {
            std::lock_guard<std::mutex> lock(impl_->state_mutex_);
            if (!impl_->is_running_.load(std::memory_order_acquire))
            {
                return;
            }

            ++impl_->run_id_;
            impl_->is_running_.store(false, std::memory_order_release);
            cs = impl_->cs_;
        }

        if (cs)
        {
            cs->emit(net::cancellation_type::all);
        }
    }

    net::any_io_executor
    ticker::get_executor() const noexcept
    {
        return impl_->executor_;
    }

    void
    ticker::set_interval(std::chrono::steady_clock::duration const& interval)
    {
        impl_->interval_.store(interval, std::memory_order_release);
    }

    bool
    ticker::is_running() const
    {
        return impl_->is_running_.load(std::memory_order_acquire);
    }

    net::awaitable<void>
    ticker::co_run()
    {
        co_await net::this_coro::reset_cancellation_state(net::enable_total_cancellation(),
                                                          net::enable_terminal_cancellation());

        co_await net::this_coro::throw_if_cancelled(false);

        auto cs = co_await net::this_coro::cancellation_state;

        // 错误处理自身抛异常时不能掀翻 run loop。
        auto report_error = [this](std::exception_ptr ep) noexcept {
            try
            {
                on_error(std::move(ep));
            }
            catch (...)
            {
            }
        };

        bool started = false;

        try
        {
            started = co_await on_start();
        }
        catch (...)
        {
            report_error(std::current_exception());
        }

        if (started)
        {
            net::steady_timer update_timer(impl_->executor_);
            while (!cs.cancelled())
            {
                // 先等待一个周期，与旧的维护循环语义保持一致。
                if (co_await sleep(update_timer, impl_->interval_.load()))
                {
                    break;
                }

                if (static_cast<bool>(cs.cancelled()))
                {
                    break;
                }

                try
                {
                    if (!co_await on_tick())
                    {
                        break;
                    }
                }
                catch (...)
                {
                    report_error(std::current_exception());
                    break;
                }
            }
        }

        try
        {
            co_await on_stop();
        }
        catch (...)
        {
            report_error(std::current_exception());
        }
    }

} // namespace httplib::util
