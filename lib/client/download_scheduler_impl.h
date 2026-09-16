#pragma once
#include "httplib/client/download_scheduler.hpp"
#include "httplib/util/async_event.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace httplib::client
{

    class download_scheduler::impl : public std::enable_shared_from_this<impl>
    {
      public:
        struct task_entry
        {
            task_id id = 0;
            std::string url;
            fs::path save_path;
            task_options opts;
            std::unique_ptr<downloader> dl;
            task_status status;
            bool pause_requested = false;
            bool cancel_requested = false;
        };

      public:
        impl(net::any_io_executor ex,
             std::shared_ptr<http_client_pool> pool,
             scheduler_config cfg);
        ~impl();

        // -- sync queries (lock + snapshot) --
        task_status get_status(task_id id) const;
        std::vector<task_status> get_all_status() const;
        std::size_t active_count() const;
        std::size_t pending_count() const;
        std::size_t total_count() const;

        // -- config snapshot --
        scheduler_config get_scheduler_config() const;

        // -- awaitable --
        net::awaitable<void> async_run();
        net::awaitable<task_status> async_wait_any();
        net::awaitable<task_status> async_wait_one(task_id id);
        net::awaitable<void> async_shutdown();

        // -- thread-safe entry points (non-blocking) --
        task_id add(std::string_view url,
                         fs::path const& save_path,
                         task_options opts);
        void cancel(task_id id);
        void cancel_all();
        void pause(task_id id);
        void resume(task_id id);
        void set_progress_callback(progress_callback cb);
        void set_state_callback(state_callback cb);
        void set_scheduler_config(scheduler_config const& cfg);

        // -- non-blocking stop (used by ~download_scheduler) --
        void request_stop();

      private:
        // Requires mtx_ to be held by the caller.
        void dispatch_pending_locked();
        void dispatch_task(std::shared_ptr<task_entry> entry);

        // Take mtx_ internally; downloader callbacks may arrive on any thread.
        void on_progress(task_id id, downloader::progress_info const& info);
        void on_state(task_id id, downloader::state st, std::string_view msg);
        void on_completion(task_id id, boost::system::error_code ec);

        std::shared_ptr<task_entry> find_task(task_id id) const;
        task_status snapshot(task_id id) const;
        std::vector<task_status> all_snapshots() const;

      private:
        net::any_io_executor ex_;
        std::shared_ptr<http_client_pool> pool_;
        scheduler_config config_;

        std::atomic<task_id> id_counter_ { 1 };

        mutable std::mutex mtx_;
        std::unordered_map<task_id, std::shared_ptr<task_entry>> tasks_;
        std::deque<task_id> pending_queue_;
        std::deque<task_id> completed_queue_;
        std::size_t running_count_ = 0;

        util::async_event scheduler_event_;
        util::async_event completed_event_;

        progress_callback progress_cb_;
        state_callback state_cb_;

        bool shutdown_requested_ = false;
    };

} // namespace httplib::client