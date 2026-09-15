#pragma once
#include "httplib/client/download_scheduler.hpp"
#include "httplib/util/async_event.hpp"
#include <boost/asio/strand.hpp>
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

        // -- sync queries (snapshot via post+future) --
        task_status get_status(task_id id) const;
        std::vector<task_status> get_all_status() const;
        std::size_t active_count() const;
        std::size_t pending_count() const;
        std::size_t total_count() const;

        // -- config snapshot --
        scheduler_config get_scheduler_config() const;

        // -- awaitable --
        net::awaitable<void> async_run();
        net::awaitable<void> async_run_all(
            std::vector<std::tuple<std::string, fs::path, task_options>> tasks);
        net::awaitable<task_status> async_wait_any();
        net::awaitable<task_status> async_wait_one(task_id id);
        net::awaitable<void> async_shutdown();

        // -- strand post helpers (thread-safe entry points) --
        task_id post_add(std::string_view url, fs::path const& save_path,
                          task_options opts);
        void post_cancel(task_id id);
        void post_cancel_all();
        void post_pause(task_id id);
        void post_resume(task_id id);
        void post_set_progress_callback(progress_callback cb);
        void post_set_state_callback(state_callback cb);
        void post_set_scheduler_config(scheduler_config const& cfg);

      private:
        // runs on strand only
        void dispatch_pending();
        void dispatch_task(std::shared_ptr<task_entry> entry);
        void on_progress(task_id id, downloader::progress_info const& info);
        void on_state(task_id id, downloader::state st, std::string_view msg);
        void on_completion(task_id id, boost::system::error_code ec);

        std::shared_ptr<task_entry> find_task(task_id id) const;

        // snapshot (called from within strand)
        task_status snapshot(task_id id) const;
        std::vector<task_status> all_snapshots() const;

      private:
        net::strand<net::any_io_executor> strand_;
        std::shared_ptr<http_client_pool> pool_;
        scheduler_config config_;

        std::atomic<task_id> id_counter_ { 1 };

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