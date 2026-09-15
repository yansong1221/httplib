#pragma once
#include "httplib/util/async_event.hpp"
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <atomic>
#include <utility>

namespace httplib::util
{

    class async_event::impl
    {
      public:
        using channel_type = net::experimental::concurrent_channel<void(boost::system::error_code)>;

        explicit impl(net::any_io_executor ex)
            : channel_(std::move(ex), 1)
        {
        }

        impl(impl const&) = delete;
        impl& operator=(impl const&) = delete;

        impl(impl&&) = delete;
        impl& operator=(impl&&) = delete;

        async_event::notify_result
        notify()
        {
            if (closed_.load(std::memory_order_acquire))
            {
                return async_event::notify_result::closed;
            }

            const bool sent = channel_.try_send(boost::system::error_code {});

            if (sent)
            {
                return async_event::notify_result::notified;
            }

            if (closed_.load(std::memory_order_acquire))
            {
                return async_event::notify_result::closed;
            }

            return async_event::notify_result::coalesced;
        }

        net::awaitable<async_event::wait_result>
        wait()
        {
            boost::system::error_code ec;

            co_await channel_.async_receive(
                boost::asio::redirect_error(
                    net::use_awaitable,
                    ec));

            if (!ec)
            {
                co_return async_event::wait_result::notified;
            }

            if (ec == boost::asio::error::operation_aborted)
            {
                if (closed_.load(std::memory_order_acquire))
                {
                    co_return async_event::wait_result::closed;
                }

                co_return async_event::wait_result::cancelled;
            }

            co_return async_event::wait_result::failed;
        }

        void
        close()
        {
            bool expected = false;

            if (!closed_.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return;
            }

            channel_.close();
        }

        bool
        is_closed() const noexcept
        {
            return closed_.load(std::memory_order_acquire);
        }

      private:
        channel_type channel_;

        std::atomic<bool> closed_ { false };
    };

} // namespace httplib::util