#include "httplib/util/ticker.hpp"
#include "httplib/util/sleep.hpp"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

namespace httplib::util
{
    namespace detail
    {
        inline static void
        set_error_code(std::exception_ptr ep, boost::system::error_code& ec)
        {
            if (ec)
            {
                return;
            }

            try
            {
                std::rethrow_exception(ep);
            }
            catch (boost::system::system_error const& e)
            {
                ec = e.code();
            }
            catch (std::exception const&)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::interrupted);
            }
            catch (...)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::interrupted);
            }
        }
    } // namespace detail

    ticker::ticker(boost::asio::any_io_executor const& executor, std::chrono::steady_clock::duration const& interval)
        : executor_(executor)
        , strand_(executor)
        , interval_(interval)
    {
    }

    ticker::~ticker() {}

    void
    ticker::start()
    {
        std::shared_ptr<boost::asio::cancellation_signal> cs;
        uint64_t generation = 0;

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (is_running_.load(std::memory_order_acquire))
            {
                return;
            }

            generation = ++run_id_;
            cs = std::make_shared<boost::asio::cancellation_signal>();
            cs_ = cs;
            is_running_.store(true, std::memory_order_release);

            // co_spawn 必须在 state_mutex_ 内完成：确保 cancellation slot 已连接后，
            // 并发的 stop() 才可能观察到 is_running_==true 并 emit，不会丢取消。
            boost::asio::co_spawn(
                strand_,
                [this, cs, generation, self = shared_from_this()]() -> boost::asio::awaitable<boost::system::error_code>
                {
                    auto ec = co_await co_run();

                    // 仅当仍是当前轮时才清除运行标志，避免旧轮清掉重启后的新轮。
                    if (run_id_.load(std::memory_order_acquire) == generation)
                    {
                        is_running_.store(false, std::memory_order_release);
                    }

                    co_return ec;
                },
                boost::asio::bind_cancellation_slot(cs->slot(), boost::asio::detached));
        }
    }

    void
    ticker::stop()
    {
        std::shared_ptr<boost::asio::cancellation_signal> cs;

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!is_running_.load(std::memory_order_acquire))
            {
                return;
            }

            // 同步失效当前轮并清除运行标志，使 stop() 后可立即 start() 重启。
            ++run_id_;
            is_running_.store(false, std::memory_order_release);
            cs = cs_;
        }

        if (cs)
        {
            // 锁外投递到 strand；post 不会 inline，避免在持锁路径上重入 on_stop。
            // 捕获当轮 signal，而非执行时再读 cs_，不会误取消重启后的新一轮。
            boost::asio::post(strand_, [cs]() { cs->emit(boost::asio::cancellation_type::all); });
        }
    }

    boost::asio::any_io_executor
    ticker::get_executor() const noexcept
    {
        return executor_;
    }

    void
    ticker::set_interval(std::chrono::steady_clock::duration const& interval)
    {
        interval_.store(interval, std::memory_order_release);
    }

    bool
    ticker::is_running() const
    {
        return is_running_.load(std::memory_order_acquire);
    }

    boost::asio::awaitable<boost::system::error_code>
    ticker::co_run()
    {
        co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation(),
                                                                  boost::asio::enable_terminal_cancellation());

        co_await boost::asio::this_coro::throw_if_cancelled(false);

        auto cs = co_await boost::asio::this_coro::cancellation_state;

        boost::system::error_code ec;
        bool started = false;

        try
        {
            started = co_await on_start();
            if (!started)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
            }
        }
        catch (...)
        {
            detail::set_error_code(std::current_exception(), ec);
        }

        if (started && !static_cast<bool>(ec))
        {
            boost::asio::steady_timer update_timer(co_await boost::asio::this_coro::executor);
            while (!cs.cancelled() && !static_cast<bool>(ec))
            {
                // 先等待一个周期，与旧的维护循环语义保持一致。
                ec = co_await sleep(update_timer, interval_.load());
                if (static_cast<bool>(ec) || static_cast<bool>(cs.cancelled()))
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
                    detail::set_error_code(std::current_exception(), ec);
                }
            }
        }

        // 统一的清理路径：无论 on_start 成功与否都调用 on_stop，子类需保证幂等。
        try
        {
            co_await on_stop();
        }
        catch (...)
        {
            detail::set_error_code(std::current_exception(), ec);
        }

        co_return ec;
    }

} // namespace httplib::util
