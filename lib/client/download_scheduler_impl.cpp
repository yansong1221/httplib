#include "download_scheduler_impl.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <algorithm>
#include <format>
#include <stdexcept>

namespace httplib::client
{

    // =========================================================================
    // construction
    // =========================================================================

    download_scheduler::impl::impl(net::any_io_executor ex,
                                   std::shared_ptr<http_client_pool> pool,
                                   scheduler_config cfg)
        : strand_(net::make_strand(ex))
        , pool_(std::move(pool))
        , config_(cfg)
        , scheduler_event_(net::any_io_executor(strand_))
        , completed_event_(net::any_io_executor(strand_))
    {
    }

    download_scheduler::impl::~impl() = default;

    // =========================================================================
    // snapshot helpers (must be called on strand)
    // =========================================================================

    std::shared_ptr<download_scheduler::impl::task_entry>
    download_scheduler::impl::find_task(task_id id) const
    {
        auto it = tasks_.find(id);
        return it != tasks_.end() ? it->second : nullptr;
    }

    download_scheduler::task_status
    download_scheduler::impl::snapshot(task_id id) const
    {
        auto entry = find_task(id);
        return entry ? entry->status : task_status {};
    }

    std::vector<download_scheduler::task_status>
    download_scheduler::impl::all_snapshots() const
    {
        std::vector<task_status> out;
        out.reserve(tasks_.size());
        for (auto const& [id, entry] : tasks_)
        {
            out.push_back(entry->status);
        }
        return out;
    }

    // =========================================================================
    // sync queries (post to strand, block on future)
    // =========================================================================

    download_scheduler::task_status
    download_scheduler::impl::get_status(task_id id) const
    {
        std::promise<task_status> promise;
        auto future = promise.get_future();
        auto self = const_cast<impl*>(this);
        net::post(strand_, [self, id, &promise]()
                  { promise.set_value(self->snapshot(id)); });
        return future.get();
    }

    std::vector<download_scheduler::task_status>
    download_scheduler::impl::get_all_status() const
    {
        std::promise<std::vector<task_status>> promise;
        auto future = promise.get_future();
        auto self = const_cast<impl*>(this);
        net::post(strand_, [self, &promise]()
                  { promise.set_value(self->all_snapshots()); });
        return future.get();
    }

    std::size_t
    download_scheduler::impl::active_count() const
    {
        std::promise<std::size_t> promise;
        auto future = promise.get_future();
        auto self = const_cast<impl*>(this);
        net::post(strand_, [self, &promise]()
                  { promise.set_value(self->running_count_); });
        return future.get();
    }

    std::size_t
    download_scheduler::impl::pending_count() const
    {
        std::promise<std::size_t> promise;
        auto future = promise.get_future();
        auto self = const_cast<impl*>(this);
        net::post(strand_, [self, &promise]()
                  { promise.set_value(self->pending_queue_.size()); });
        return future.get();
    }

    std::size_t
    download_scheduler::impl::total_count() const
    {
        std::promise<std::size_t> promise;
        auto future = promise.get_future();
        auto self = const_cast<impl*>(this);
        net::post(strand_, [self, &promise]()
                  { promise.set_value(self->tasks_.size()); });
        return future.get();
    }

    download_scheduler::scheduler_config
    download_scheduler::impl::get_scheduler_config() const
    {
        std::promise<scheduler_config> promise;
        auto future = promise.get_future();
        auto self = const_cast<impl*>(this);
        net::post(strand_, [self, &promise]()
                  { promise.set_value(self->config_); });
        return future.get();
    }

    // =========================================================================
    // strand post helpers (thread-safe entry points)
    // =========================================================================

    download_scheduler::task_id
    download_scheduler::impl::post_add(std::string_view url,
                                       fs::path const& save_path,
                                       task_options opts)
    {
        auto id = id_counter_.fetch_add(1, std::memory_order_relaxed);

        std::promise<task_id> promise;
        auto future = promise.get_future();

        net::post(strand_,
                  [this, id, url_str = std::string(url), save_path, opts = std::move(opts), &promise]() mutable
                  {
                      auto entry = std::make_shared<task_entry>();
                      entry->id = id;
                      entry->url = url_str;
                      entry->save_path = save_path;
                      entry->opts = std::move(opts);
                      entry->status.id = id;
                      entry->status.url = entry->url;
                      entry->status.save_path = save_path;

                      tasks_[id] = entry;
                      pending_queue_.push_back(id);
                      entry->status.state = downloader::state::idle;

                      promise.set_value(id);
                      scheduler_event_.signal();
                  });

        return future.get();
    }

    void
    download_scheduler::impl::post_cancel(task_id id)
    {
        net::post(strand_,
                  [this, id]()
                  {
                      auto entry = find_task(id);
                      if (!entry)
                          return;
                      entry->cancel_requested = true;

                      // If the task is running or about to run, cancel the
                      // underlying downloader; its coroutine will settle.
                      if (entry->dl)
                      {
                          entry->dl->cancel();
                          scheduler_event_.signal();
                          return;
                      }

                      // Otherwise it is still pending (never dispatched):
                      // finalise it immediately so the terminal state lands
                      // regardless of whether a concurrency slot frees up.
                      auto it = std::find(pending_queue_.begin(), pending_queue_.end(), id);
                      if (it != pending_queue_.end())
                      {
                          pending_queue_.erase(it);
                          entry->status.state = downloader::state::cancelled;
                          entry->status.message = "cancelled before start";
completed_queue_.push_back(id);
                           scheduler_event_.signal();
                           completed_event_.signal();
                       }
                   });
    }

    void
    download_scheduler::impl::post_cancel_all()
    {
        net::post(strand_,
                  [this]()
                  {
                      for (auto& [id, entry] : tasks_)
                      {
                          entry->cancel_requested = true;
                          if (entry->dl)
                          {
                              entry->dl->cancel();
                          }
                          else
                          {
                              auto it = std::find(pending_queue_.begin(), pending_queue_.end(), id);
                              if (it != pending_queue_.end())
                              {
                                  pending_queue_.erase(it);
                                  entry->status.state = downloader::state::cancelled;
                                  entry->status.message = "cancelled before start";
                                  completed_queue_.push_back(id);
                                  completed_event_.signal();
                              }
                          }
                      }
                      scheduler_event_.signal();
                  });
    }

    void
    download_scheduler::impl::post_pause(task_id id)
    {
        net::post(strand_,
                  [this, id]()
                  {
                      auto entry = find_task(id);
                      if (!entry)
                          return;
                      entry->pause_requested = true;
                      if (entry->dl)
                      {
                          entry->dl->pause();
                      }
                      scheduler_event_.signal();
                  });
    }

    void
    download_scheduler::impl::post_resume(task_id id)
    {
        net::post(strand_,
                  [this, id]()
                  {
                      auto entry = find_task(id);
                      if (!entry)
                          return;
                      entry->pause_requested = false;
                      if (entry->dl)
                      {
                          entry->dl->resume();
                      }
                      scheduler_event_.signal();
                  });
    }

    void
    download_scheduler::impl::post_set_progress_callback(progress_callback cb)
    {
        net::post(strand_,
                  [this, cb = std::move(cb)]() mutable
                  { progress_cb_ = std::move(cb); });
    }

    void
    download_scheduler::impl::post_set_state_callback(state_callback cb)
    {
        net::post(strand_,
                  [this, cb = std::move(cb)]() mutable
                  { state_cb_ = std::move(cb); });
    }

    void
    download_scheduler::impl::post_set_scheduler_config(scheduler_config const& cfg)
    {
        net::post(strand_,
                  [this, cfg]()
                  {
                      config_ = cfg;
                      scheduler_event_.signal();
                  });
    }

    // =========================================================================
    // dispatch (runs on strand)
    // =========================================================================

    void
    download_scheduler::impl::dispatch_pending()
    {
        std::size_t to_dispatch = (config_.max_concurrent > running_count_)
                                      ? (config_.max_concurrent - running_count_)
                                      : 0;

        std::deque<task_id> skipped;
        std::size_t dispatched = 0;

        while (!pending_queue_.empty() && dispatched < to_dispatch)
        {
            auto id = pending_queue_.front();
            pending_queue_.pop_front();
            auto it = tasks_.find(id);
            if (it == tasks_.end())
                continue;
            auto& entry = it->second;
            if (entry->cancel_requested)
            {
                // Lazy-delete: cancel a pending task that was never dispatched.
                entry->status.state = downloader::state::cancelled;
                entry->status.message = "cancelled before start";
                completed_queue_.push_back(id);
                completed_event_.signal();
                continue;
            }
            if (entry->pause_requested)
            {
                skipped.push_back(id);
                continue;
            }
            running_count_++;
            dispatch_task(entry);
            dispatched++;
        }

        // re-insert skipped items at front (preserving relative order)
        for (auto sit = skipped.rbegin(); sit != skipped.rend(); ++sit)
        {
            pending_queue_.push_front(*sit);
        }
    }

    void
    download_scheduler::impl::dispatch_task(std::shared_ptr<task_entry> entry)
    {
        auto weak_self = weak_from_this();
        auto url = entry->url;
        auto save_path = entry->save_path;
        auto headers = entry->opts.headers;
        auto dl_config = entry->opts.dl_config;
        auto id = entry->id;

        net::co_spawn(
            strand_,
            [weak_self, entry, url, save_path, headers, dl_config, id]() mutable
                -> net::awaitable<void>
            {
                auto self = weak_self.lock();
                if (!self)
                    co_return;

                entry->dl = std::make_unique<downloader>(net::any_io_executor(self->strand_), self->pool_);
                entry->dl->set_config(dl_config);
                entry->dl->set_progress_callback(
                    [weak_self, id](downloader::progress_info const& info)
                    {
                        auto self = weak_self.lock();
                        if (self)
                            self->on_progress(id, info);
                    });
                entry->dl->set_state_callback(
                    [weak_self, id](downloader::state st, std::string_view msg)
                    {
                        auto self = weak_self.lock();
                        if (self)
                            self->on_state(id, st, msg);
                    });

                entry->status.state = downloader::state::connecting;

                auto ec = co_await entry->dl->async_download(url, save_path, headers);

                // still on strand
                auto self2 = weak_self.lock();
                if (self2)
                    self2->on_completion(id, ec);
            },
            net::detached);
    }

    // =========================================================================
    // callbacks (runs on strand)
    // =========================================================================

    void
    download_scheduler::impl::on_progress(task_id id,
                                          downloader::progress_info const& info)
    {
        // callback_mutex_ guards the callback; copy it, then invoke outside state
        progress_callback cb;
        task_status ts;
        {
            auto entry = find_task(id);
            if (!entry)
                return;
            cb = progress_cb_;
            ts = entry->status;
            ts.total_bytes = info.total_bytes;
            ts.downloaded_bytes = info.downloaded_bytes;
            ts.speed_bytes_per_sec = info.speed_bytes_per_sec;
        }
        if (cb)
        {
            try
            {
                cb(ts);
            }
            catch (...)
            {
            }
        }
    }

    void
    download_scheduler::impl::on_state(task_id id,
                                       downloader::state st,
                                       std::string_view msg)
    {
        state_callback cb;
        task_status ts;
        {
            auto entry = find_task(id);
            if (!entry)
                return;
            cb = state_cb_;
            entry->status.state = st;
            entry->status.message = std::string(msg);
            ts = entry->status;
        }
        if (cb)
        {
            try
            {
                cb(ts);
            }
            catch (...)
            {
            }
        }
    }

    void
    download_scheduler::impl::on_completion(task_id id,
                                           boost::system::error_code ec)
    {
        auto entry = find_task(id);
        if (!entry)
            return;

        running_count_--;

        entry->status.message = ec ? ec.message() : "";

        completed_queue_.push_back(id);
        scheduler_event_.signal();
        completed_event_.signal();

        dispatch_pending();
    }

    // =========================================================================
    // awaitable: async_run
    // =========================================================================

    net::awaitable<void>
    download_scheduler::impl::async_run()
    {
        co_await net::dispatch(strand_, net::use_awaitable);

        for (;;)
        {
            dispatch_pending();

            // all done? (no pending + no running)
            if (shutdown_requested_ && pending_queue_.empty() && running_count_ == 0)
                co_return;
            if (!shutdown_requested_ && pending_queue_.empty() && running_count_ == 0 && tasks_.empty())
                co_return;

            co_await scheduler_event_.wait();
        }
    }

    net::awaitable<void>
    download_scheduler::impl::async_run_all(
        std::vector<std::tuple<std::string, fs::path, task_options>> tasks)
    {
        for (auto& [url, path, opts] : tasks)
        {
            post_add(url, path, std::move(opts));
        }
        co_await async_run();
    }

    // =========================================================================
    // awaitable: async_wait_any / async_wait_one
    // =========================================================================

    net::awaitable<download_scheduler::task_status>
    download_scheduler::impl::async_wait_any()
    {
        co_await net::dispatch(strand_, net::use_awaitable);

        for (;;)
        {
            if (!completed_queue_.empty())
            {
                auto id = completed_queue_.front();
                completed_queue_.pop_front();
                co_return snapshot(id);
            }
            co_await completed_event_.wait();
        }
    }

    net::awaitable<download_scheduler::task_status>
    download_scheduler::impl::async_wait_one(task_id id)
    {
        co_await net::dispatch(strand_, net::use_awaitable);
        for (;;)
        {
            // check if already in completed queue
            for (auto it = completed_queue_.begin(); it != completed_queue_.end(); ++it)
            {
                if (*it == id)
                {
                    completed_queue_.erase(it);
                    co_return snapshot(id);
                }
            }

            // check if task is terminal (completed before wait called)
            auto entry = find_task(id);
            if (!entry)
            {
                task_status ts {};
                ts.id = id;
                ts.state = downloader::state::cancelled;
                ts.message = "task not found";
                co_return ts;
            }
            if (entry->status.state == downloader::state::completed ||
                entry->status.state == downloader::state::failed ||
                entry->status.state == downloader::state::cancelled)
            {
                co_return entry->status;
            }

            co_await completed_event_.wait();
        }
    }

    // =========================================================================
    // awaitable: async_shutdown
    // =========================================================================

    net::awaitable<void>
    download_scheduler::impl::async_shutdown()
    {
        // mark shutdown
        co_await net::post(strand_, net::use_awaitable);

        shutdown_requested_ = true;

        // cancel all pending (lazy delete)
        for (auto& id : pending_queue_)
        {
            auto entry = find_task(id);
            if (entry)
                entry->cancel_requested = true;
        }

        // cancel all running
        for (auto& [id, entry] : tasks_)
        {
            if (entry->dl)
                entry->dl->cancel();
        }

        scheduler_event_.signal();

        // drain until everything settled
        for (;;)
        {
            dispatch_pending();
            if (pending_queue_.empty() && running_count_ == 0)
                co_return;
            co_await scheduler_event_.wait();
        }
    }

} // namespace httplib::client