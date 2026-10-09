#include "download_scheduler_impl.h"
#include <algorithm>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/errc.hpp>
#include <optional>

namespace httplib::client
{

    // =========================================================================
    // construction
    // =========================================================================

    download_scheduler::impl::impl(net::any_io_executor ex,
                                   std::shared_ptr<http_client_pool> pool,
                                   scheduler_config cfg)
        : ex_(std::move(ex))
        , pool_(std::move(pool))
        , config_(cfg)
        , scheduler_event_(ex_)
        , completed_event_(ex_)
    {
    }

    download_scheduler::impl::~impl() = default;

    // =========================================================================
    // state helpers (caller must hold mtx_)
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
    // sync queries (lock + snapshot, safe from any thread)
    // =========================================================================

    download_scheduler::task_status
    download_scheduler::impl::get_status(task_id id) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return snapshot(id);
    }

    std::vector<download_scheduler::task_status>
    download_scheduler::impl::get_all_status() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return all_snapshots();
    }

    std::size_t
    download_scheduler::impl::active_count() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return running_count_;
    }

    std::size_t
    download_scheduler::impl::pending_count() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return pending_queue_.size();
    }

    std::size_t
    download_scheduler::impl::total_count() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return tasks_.size();
    }

    std::size_t
    download_scheduler::impl::clear_finished()
    {
        std::lock_guard<std::mutex> lk(mtx_);

        std::vector<task_id> finished;
        for (auto const& [id, entry] : tasks_)
        {
            auto st = entry->status.state;
            if (st == downloader::state::completed || st == downloader::state::failed
                || st == downloader::state::cancelled)
            {
                finished.push_back(id);
            }
        }
        for (auto id : finished)
        {
            tasks_.erase(id);
        }

        if (!finished.empty())
        {
            completed_queue_.erase(
                std::remove_if(completed_queue_.begin(),
                               completed_queue_.end(),
                               [&](task_id id)
                               { return std::find(finished.begin(), finished.end(), id) != finished.end(); }),
                completed_queue_.end());
        }
        return finished.size();
    }

    download_scheduler::scheduler_config
    download_scheduler::impl::get_scheduler_config() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return config_;
    }

    // =========================================================================
    // thread-safe entry points
    // =========================================================================

    download_scheduler::task_id
    download_scheduler::impl::add(std::string_view url, fs::path const& save_path, task_options opts)
    {
        auto id = id_counter_.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (shutdown_requested_)
            {
                return id;
            }

            auto entry = std::make_shared<task_entry>();
            entry->id = id;
            entry->url = std::string(url);
            entry->save_path = save_path;
            entry->opts = std::move(opts);
            entry->status.id = id;
            entry->status.url = entry->url;
            entry->status.save_path = save_path;
            entry->status.state = downloader::state::idle;

            tasks_[id] = entry;
            pending_queue_.push_back(id);
        }
        scheduler_event_.notify_all();

        return id;
    }

    void
    download_scheduler::impl::cancel(task_id id)
    {
        bool completed_changed = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto entry = find_task(id);
            if (!entry)
            {
                return;
            }
            entry->cancel_requested = true;

            // If the task is running or about to run, cancel the underlying
            // downloader; its coroutine will settle.
            if (entry->dl)
            {
                entry->dl->cancel();
            }
            else
            {
                // Otherwise it is still pending (never dispatched): finalise it
                // immediately so the terminal state lands regardless of whether
                // a concurrency slot frees up.
                auto it = std::find(pending_queue_.begin(), pending_queue_.end(), id);
                if (it != pending_queue_.end())
                {
                    pending_queue_.erase(it);
                    entry->status.state = downloader::state::cancelled;
                    completed_queue_.push_back(id);
                    completed_changed = true;
                }
            }
        }
        scheduler_event_.notify_all();
        if (completed_changed)
        {
            completed_event_.notify_all();
        }
    }

    void
    download_scheduler::impl::cancel_all()
    {
        bool completed_changed = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
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
                        completed_queue_.push_back(id);
                        completed_changed = true;
                    }
                }
            }
        }
        scheduler_event_.notify_all();
        if (completed_changed)
        {
            completed_event_.notify_all();
        }
    }

    void
    download_scheduler::impl::pause(task_id id)
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto entry = find_task(id);
            if (!entry)
            {
                return;
            }
            entry->pause_requested = true;
            if (entry->dl)
            {
                entry->dl->pause();
            }
        }
        scheduler_event_.notify_all();
    }

    void
    download_scheduler::impl::resume(task_id id)
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto entry = find_task(id);
            if (!entry)
            {
                return;
            }
            entry->pause_requested = false;
            if (entry->dl)
            {
                entry->dl->resume();
            }
        }
        scheduler_event_.notify_all();
    }

    void
    download_scheduler::impl::set_progress_callback(progress_callback cb)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        progress_cb_ = std::move(cb);
    }

    void
    download_scheduler::impl::set_state_callback(state_callback cb)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_cb_ = std::move(cb);
    }

    void
    download_scheduler::impl::set_scheduler_config(scheduler_config const& cfg)
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            config_ = cfg;
        }
        scheduler_event_.notify_all();
    }

    void
    download_scheduler::impl::set_cache(std::shared_ptr<cache> c)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cache_ = std::move(c);
    }

    std::shared_ptr<cache>
    download_scheduler::impl::get_cache() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return cache_;
    }

    void
    download_scheduler::impl::request_stop()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            shutdown_requested_ = true;
            for (auto& id : pending_queue_)
            {
                if (auto entry = find_task(id); entry)
                {
                    entry->cancel_requested = true;
                }
            }
            for (auto& [id, entry] : tasks_)
            {
                if (entry->dl)
                {
                    entry->dl->cancel();
                }
            }
        }
        scheduler_event_.notify_all();
    }

    // =========================================================================
    // dispatch (caller must hold mtx_)
    // =========================================================================

    void
    download_scheduler::impl::dispatch_pending_locked()
    {
        std::size_t to_dispatch
            = (config_.max_concurrent > running_count_) ? (config_.max_concurrent - running_count_) : 0;

        std::deque<task_id> skipped;
        std::size_t dispatched = 0;

        while (!pending_queue_.empty() && dispatched < to_dispatch)
        {
            auto id = pending_queue_.front();
            pending_queue_.pop_front();
            auto it = tasks_.find(id);
            if (it == tasks_.end())
            {
                continue;
            }
            auto& entry = it->second;
            if (entry->cancel_requested)
            {
                entry->status.state = downloader::state::cancelled;
                completed_queue_.push_back(id);
                completed_event_.notify_all();
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

        // Re-insert skipped items at front (preserving relative order).
        for (auto sit = skipped.rbegin(); sit != skipped.rend(); ++sit)
        {
            pending_queue_.push_front(*sit);
        }
    }

    void
    download_scheduler::impl::dispatch_task(std::shared_ptr<task_entry> entry)
    {
        // mtx_ is held by the caller.
        auto weak_self = weak_from_this();
        auto ex = ex_;
        auto url = entry->url;
        auto save_path = entry->save_path;
        auto headers = entry->opts.headers;
        auto dl_config = entry->opts.dl_config;
        auto id = entry->id;

        auto dl = std::make_unique<downloader>(ex, pool_);
        dl->set_config(dl_config);
        if (cache_)
        {
            dl->set_cache(cache_);
        }
        dl->set_progress_callback(
            [weak_self, id](downloader::progress_info const& info)
            {
                auto self = weak_self.lock();
                if (self)
                {
                    self->on_progress(id, info);
                }
            });
        dl->set_state_callback(
            [weak_self, id](downloader::state st, boost::system::error_code ec)
            {
                auto self = weak_self.lock();
                if (self)
                {
                    self->on_state(id, st, ec);
                }
            });
        entry->dl = std::move(dl);
        entry->status.state = downloader::state::connecting;

        net::co_spawn(
            ex,
            [weak_self, entry, url, save_path, headers, id, ex]() mutable -> net::awaitable<void>
            {
                // Hop off the caller's stack first: dispatch_task runs while the
                // scheduler mutex is held, so we must not touch scheduler state
                // (or let the downloader fire callbacks synchronously) inline.
                co_await net::post(ex, net::use_awaitable);

                auto self = weak_self.lock();
                if (!self)
                {
                    co_return;
                }

                boost::system::error_code ec;
                try
                {
                    ec = co_await entry->dl->async_download(url, save_path, headers);
                }
                catch (...)
                {
                    // A throwing downloader must still settle its task, otherwise
                    // the concurrency slot is leaked forever.
                    ec = boost::system::errc::make_error_code(boost::system::errc::io_error);
                }

                auto self2 = weak_self.lock();
                if (self2)
                {
                    self2->on_completion(id, ec);
                }
            },
            net::detached);
    }

    // =========================================================================
    // callbacks (downloader may invoke on any io thread)
    // =========================================================================

    void
    download_scheduler::impl::on_progress(task_id id, downloader::progress_info const& info)
    {
        progress_callback cb;
        task_status ts;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto entry = find_task(id);
            if (!entry)
            {
                return;
            }
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
    download_scheduler::impl::on_state(task_id id, downloader::state st, boost::system::error_code ec)
    {
        state_callback cb;
        task_status ts;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto entry = find_task(id);
            if (!entry)
            {
                return;
            }
            cb = state_cb_;
            entry->status.state = st;
            ts = entry->status;
        }
        if (cb)
        {
            try
            {
                cb(ts, ec);
            }
            catch (...)
            {
            }
        }
    }

    void
    download_scheduler::impl::on_completion(task_id id, boost::system::error_code ec)
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto entry = find_task(id);
            if (!entry)
            {
                return;
            }

            running_count_--;

            entry->status.error = ec;
            completed_queue_.push_back(id);
            dispatch_pending_locked();
        }
        scheduler_event_.notify_all();
        completed_event_.notify_all();
    }

    // =========================================================================
    // awaitable: async_run
    // =========================================================================

    net::awaitable<void>
    download_scheduler::impl::async_run()
    {
        auto self = shared_from_this();
        co_return co_await net::co_spawn(
            ex_,
            [&]() -> net::awaitable<void>
            {
                for (;;)
                {
                    bool done = false;
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        dispatch_pending_locked();

                        // All pending queued and running tasks drained.
                        if (pending_queue_.empty() && running_count_ == 0)
                        {
                            done = tasks_.empty() || shutdown_requested_;
                        }
                    }
                    if (done)
                    {
                        co_return;
                    }

                    co_await scheduler_event_.wait();
                }
            },
            net::use_awaitable);
    }
    std::future<void>
    download_scheduler::impl::run()
    {
        return net::co_spawn(
            ex_,
            [self = shared_from_this()]() -> net::awaitable<void>
            {
                co_await self->async_run();
                co_return;
            },
            net::use_future);
    }

    // =========================================================================
    // awaitable: async_wait_any / async_wait_one
    // =========================================================================

    net::awaitable<download_scheduler::task_status>
    download_scheduler::impl::async_wait_any()
    {
        auto self = shared_from_this();
        co_return co_await net::co_spawn(
            ex_,
            [&]() -> net::awaitable<download_scheduler::task_status>
            {
                for (;;)
                {
                    std::optional<task_status> result;
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        if (!completed_queue_.empty())
                        {
                            auto id = completed_queue_.front();
                            completed_queue_.pop_front();
                            result = snapshot(id);
                        }
                    }
                    if (result)
                    {
                        co_return *result;
                    }

                    co_await completed_event_.wait();
                }
            },
            net::use_awaitable);
    }

    net::awaitable<download_scheduler::task_status>
    download_scheduler::impl::async_wait_one(task_id id)
    {
        auto self = shared_from_this();
        co_return co_await net::co_spawn(
            ex_,
            [&]() -> net::awaitable<download_scheduler::task_status>
            {
                for (;;)
                {
                    std::optional<task_status> result;
                    {
                        std::lock_guard<std::mutex> lk(mtx_);

                        // A still-unconsumed terminal completion, if present.
                        for (auto it = completed_queue_.begin(); it != completed_queue_.end(); ++it)
                        {
                            if (*it == id)
                            {
                                completed_queue_.erase(it);
                                result = snapshot(id);
                                break;
                            }
                        }

                        if (!result)
                        {
                            auto entry = find_task(id);
                            if (!entry)
                            {
                                task_status ts {};
                                ts.id = id;
                                ts.state = downloader::state::cancelled;
                                ts.error = boost::system::errc::make_error_code(
                                    boost::system::errc::no_such_file_or_directory);
                                result = ts;
                            }
                            else if (entry->status.state == downloader::state::completed
                                     || entry->status.state == downloader::state::failed
                                     || entry->status.state == downloader::state::cancelled)
                            {
                                result = entry->status;
                            }
                        }
                    }
                    if (result)
                    {
                        co_return *result;
                    }

                    co_await completed_event_.wait();
                }
            },
            net::use_awaitable);
    }

    // =========================================================================
    // awaitable: async_shutdown
    // =========================================================================

    net::awaitable<void>
    download_scheduler::impl::async_shutdown()
    {
        auto self = shared_from_this();
        co_return co_await net::co_spawn(
            ex_,
            [&]() -> net::awaitable<void>
            {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    shutdown_requested_ = true;

                    for (auto& id : pending_queue_)
                    {
                        if (auto entry = find_task(id); entry)
                        {
                            entry->cancel_requested = true;
                        }
                    }

                    for (auto& [id, entry] : tasks_)
                    {
                        if (entry->dl)
                        {
                            entry->dl->cancel();
                        }
                    }
                }
                scheduler_event_.notify_all();

                for (;;)
                {
                    bool drained = false;
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        dispatch_pending_locked();
                        drained = pending_queue_.empty() && running_count_ == 0;
                    }
                    if (drained)
                    {
                        co_return;
                    }

                    co_await scheduler_event_.wait();
                }
            },
            net::use_awaitable);
    }

} // namespace httplib::client