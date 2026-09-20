#pragma once
#include "httplib/config.hpp"
#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <memory>
#include <mutex>

namespace httplib::util
{
    class HTTPLIB_API ticker : public std::enable_shared_from_this<ticker>
    {
      public:
        explicit ticker(net::any_io_executor const& executor,
                        std::chrono::steady_clock::duration const& interval = std::chrono::milliseconds(500));
        virtual ~ticker();

      public:
        virtual void start();
        virtual void stop();

        net::any_io_executor get_executor() const noexcept;

        void set_interval(std::chrono::steady_clock::duration const& interval);
        bool is_running() const;

      protected:
        virtual net::awaitable<bool>
        on_start()
        {
            co_return true;
        }
        virtual net::awaitable<bool> on_tick() = 0;
        virtual net::awaitable<void>
        on_stop()
        {
            co_return;
        }

      private:
        net::awaitable<boost::system::error_code> co_run();

      private:
        net::any_io_executor executor_;

        std::atomic<std::chrono::steady_clock::duration> interval_;
        std::atomic<bool> is_running_ { false };
        std::atomic<uint64_t> run_id_ { 0 };

        std::mutex state_mutex_;
        std::shared_ptr<net::cancellation_signal> cs_;
    };
} // namespace httplib::util
