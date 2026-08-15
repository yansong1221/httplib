#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/mysql/connection_pool.hpp"
#include "httplib/mysql/session.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace httplib::mysql
{

    struct connection_pool::impl : public std::enable_shared_from_this<impl>
    {
        impl(net::any_io_executor ex, pool_params cfg);
        ~impl();

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> l);

        void start();
        net::awaitable<session_handle> async_acquire(std::chrono::steady_clock::duration wait_timeout);
        void release_session(std::unique_ptr<session> sess);
        void stop();

        size_t active_count() const;
        size_t idle_count() const;
        size_t total_count() const;

        net::any_io_executor
        get_executor() noexcept
        {
            return ex_;
        }

      private:
        using waiters_list = std::deque<std::weak_ptr<net::steady_timer>>;

        std::unique_ptr<session> try_pop_idle();
        net::awaitable<std::unique_ptr<session>> try_pop_validated();
        void wake_one_waiter();
        void push_idle(std::unique_ptr<session> sess);
        net::awaitable<std::unique_ptr<session>> create_session();
        net::awaitable<void> co_init_and_maintain(uint64_t epoch);
        net::awaitable<void> co_maintain(uint64_t epoch);

        mutable std::mutex mutex_;
        std::vector<std::unique_ptr<session>> idle_;
        size_t active_count_ = 0;
        waiters_list waiters_;

        net::any_io_executor ex_;
        pool_params cfg_;

        std::shared_ptr<spdlog::logger> default_logger_;
        std::shared_ptr<spdlog::logger> custom_logger_;

        std::atomic<bool> stopped_ { true };
        std::atomic<uint64_t> epoch_ { 0 };
        net::steady_timer maintain_timer_;
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
