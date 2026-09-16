#pragma once
#include "httplib/client/client_fwd.hpp"
#include "httplib/client/downloader.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::client
{

    class HTTPLIB_API download_scheduler
    {
      public:
        using task_id = std::uint64_t;

        struct scheduler_config
        {
            std::size_t max_concurrent = 8;
        };

        struct task_options
        {
            downloader::config dl_config;
            http::fields headers;
        };

        struct task_status
        {
            task_id id = 0;

            std::string url;
            fs::path save_path;

            downloader::state state = downloader::state::idle;

            std::uint64_t total_bytes = 0;
            std::uint64_t downloaded_bytes = 0;
            std::uint64_t speed_bytes_per_sec = 0;

            boost::system::error_code error;
        };

        using progress_callback = std::function<void(task_status const&)>;
        using state_callback = std::function<void(task_status const&, boost::system::error_code)>;

      public:
        explicit download_scheduler(net::any_io_executor ex,
                                    std::shared_ptr<http_client_pool> pool,
                                    scheduler_config cfg = {});
        ~download_scheduler();

        download_scheduler(download_scheduler const&) = delete;
        download_scheduler& operator=(download_scheduler const&) = delete;

        download_scheduler(download_scheduler&&) = delete;
        download_scheduler& operator=(download_scheduler&&) = delete;

        // -- task lifecycle --

        task_id add(std::string_view url, fs::path const& save_path, task_options opts = {});

        void cancel(task_id id);
        void cancel_all();

        void pause(task_id id);
        void resume(task_id id);

        // -- status snapshot --

        task_status get_status(task_id id) const;
        std::vector<task_status> get_all_status() const;

        std::size_t active_count() const;
        std::size_t pending_count() const;
        std::size_t total_count() const;

        // -- callbacks --

        void set_progress_callback(progress_callback cb);
        void set_state_callback(state_callback cb);

        // -- config --

        void set_scheduler_config(scheduler_config const& cfg);
        scheduler_config get_scheduler_config() const;

        // -- cache --

        /// Attach a shared cache that every task's downloader will use.
        /// Tasks scheduled after this point share the same cache instance.
        void set_cache(std::shared_ptr<cache> c);
        std::shared_ptr<cache> get_cache() const;

        void run();

        // -- awaitable --

        /// Drive all tasks until every submitted task reaches a terminal state
        /// (completed / failed / cancelled).  New tasks may be added while
        /// this coroutine is running.
        net::awaitable<void> async_run();

        /// Wait for the next task to reach a terminal state.
        net::awaitable<task_status> async_wait_any();

        /// Wait for a specific task to reach a terminal state.
        net::awaitable<task_status> async_wait_one(task_id id);

        /// Gracefully shut down: cancel pending, cancel running, drain events.
        net::awaitable<void> async_shutdown();

      private:
        class impl;
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::client