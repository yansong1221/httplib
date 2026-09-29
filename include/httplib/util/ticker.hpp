#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <exception>
#include <memory>

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

        /// run loop 内部抛出的异常都会交给这里，然后循环停止。默认忽略。
        virtual void
        on_error(std::exception_ptr ep)
        {
            (void)ep;
        }

      private:
        net::awaitable<void> co_run();

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };
} // namespace httplib::util
