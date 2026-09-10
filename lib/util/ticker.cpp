#include "httplib/util/ticker.hpp"
#include "httplib/util/sleep.hpp"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>

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
            catch (std::exception const& e)
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
        if (is_running_.exchange(true))
        {
            return;
        }
        auto cs = std::make_shared<boost::asio::cancellation_signal>();

        boost::asio::co_spawn(
            strand_,
            [this, cs, self = shared_from_this()]() -> boost::asio::awaitable<boost::system::error_code>
            {
                boost::system::error_code ec;

                ec = co_await co_run();
                is_running_ = false;

                co_return ec;
            },
            boost::asio::bind_cancellation_slot(cs->slot(), boost::asio::detached));

        cs_ = cs;
    }

    void
    ticker::stop()
    {
        boost::asio::dispatch(strand_,
                              [this, self = shared_from_this()]()
                              {
                                  if (!is_running_)
                                  {
                                      return;
                                  }
                                  if (auto cs = cs_; cs)
                                  {
                                      cs->emit(boost::asio::cancellation_type::all);
                                  }
                              });
    }

    boost::asio::any_io_executor
    ticker::get_executor() const noexcept
    {
        return executor_;
    }
    void
    ticker::set_interval(std::chrono::steady_clock::duration const& interval)
    {
        boost::asio::dispatch(strand_, [this, interval, self = shared_from_this()]() { interval_ = interval; });
    }
    bool
    ticker::is_running() const
    {
        return is_running_;
    }

    boost::asio::awaitable<boost::system::error_code>
    ticker::co_run()
    {
        co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation(),
                                                                  boost::asio::enable_terminal_cancellation());

        co_await boost::asio::this_coro::throw_if_cancelled(false);

        auto cs = co_await boost::asio::this_coro::cancellation_state;

        try
        {
            if (!co_await on_start())
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
            }
        }
        catch (...)
        {
            boost::system::error_code ec;
            detail::set_error_code(std::current_exception(), ec);
            co_return ec;
        }

        boost::system::error_code ec;
        boost::asio::steady_timer update_timer(co_await boost::asio::this_coro::executor);
        for (; !cs.cancelled() && !ec; ec = co_await sleep(update_timer, interval_))
        {
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