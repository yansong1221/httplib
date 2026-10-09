#include "progress_tracker.hpp"
#include <algorithm>

namespace httplib::client
{

    progress_tracker::progress_tracker(net::any_io_executor ex)
        : executor_(ex)
        , pause_event_(ex)
        , progress_cb_(nullptr)
        , state_cb_(nullptr)
        , state_(downloader::state::idle)
        , total_bytes_(0)
        , downloaded_bytes_(0)
        , active_segments_(0)
        , total_segments_(1)
        , paused_(false)
    {
    }

    void
    progress_tracker::set_progress_callback(downloader::progress_callback cb)
    {
        progress_cb_.store(std::make_shared<downloader::progress_callback>(std::move(cb)));
    }

    void
    progress_tracker::set_state_callback(downloader::state_callback cb)
    {
        state_cb_.store(std::make_shared<downloader::state_callback>(std::move(cb)));
    }

    void
    progress_tracker::start(std::uint64_t total_bytes, int total_segments)
    {
        std::lock_guard lk(mutex_);
        total_bytes_ = total_bytes;
        downloaded_bytes_ = 0;
        total_segments_ = total_segments;
        active_segments_ = total_segments;
        progress_start_ = std::chrono::steady_clock::now();
        seg_downloaded_.assign(static_cast<std::size_t>(total_segments), 0);
    }

    void
    progress_tracker::set_downloaded(std::uint64_t n)
    {
        std::lock_guard lk(mutex_);
        downloaded_bytes_ = n;
    }

    void
    progress_tracker::update(std::uint64_t delta_bytes)
    {
        auto cb = progress_cb_.load();
        if (!cb || !*cb || delta_bytes == 0)
        {
            return;
        }
        downloader::progress_info info;
        {
            std::lock_guard lk(mutex_);
            downloaded_bytes_ += delta_bytes;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration<double>(now - progress_start_);
            info.total_bytes = total_bytes_;
            info.downloaded_bytes = downloaded_bytes_;
            info.active_segments = active_segments_;
            info.total_segments = total_segments_;
            if (elapsed.count() > 0.0 && downloaded_bytes_ > 0)
            {
                info.speed_bytes_per_sec
                    = static_cast<std::uint64_t>(static_cast<double>(downloaded_bytes_) / elapsed.count());
                if (total_bytes_ > 0 && info.speed_bytes_per_sec > 0)
                {
                    auto remaining = total_bytes_ - downloaded_bytes_;
                    info.eta = std::chrono::seconds(static_cast<int64_t>(
                        static_cast<double>(remaining) / static_cast<double>(info.speed_bytes_per_sec)));
                }
            }
        }
        (*cb)(info);
    }

    void
    progress_tracker::record_segment_bytes(int index, std::uint64_t n)
    {
        std::lock_guard lk(mutex_);
        if (index >= 0 && static_cast<std::size_t>(index) < seg_downloaded_.size())
        {
            seg_downloaded_[static_cast<std::size_t>(index)] += n;
        }
    }

    std::uint64_t
    progress_tracker::segment_bytes(int index) const
    {
        std::lock_guard lk(mutex_);
        if (index >= 0 && static_cast<std::size_t>(index) < seg_downloaded_.size())
        {
            return seg_downloaded_[static_cast<std::size_t>(index)];
        }
        return 0;
    }

    void
    progress_tracker::reset_segment(int index)
    {
        std::lock_guard lk(mutex_);
        if (index >= 0 && static_cast<std::size_t>(index) < seg_downloaded_.size())
        {
            seg_downloaded_[static_cast<std::size_t>(index)] = 0;
        }
    }

    void
    progress_tracker::set_segment_bytes(int index, std::uint64_t n)
    {
        std::lock_guard lk(mutex_);
        if (index >= 0)
        {
            auto uidx = static_cast<std::size_t>(index);
            if (uidx >= seg_downloaded_.size())
            {
                seg_downloaded_.resize(uidx + 1, 0);
            }
            seg_downloaded_[uidx] = n;
        }
    }

    void
    progress_tracker::set_all_segment_bytes(std::vector<std::uint64_t> const& v)
    {
        std::lock_guard lk(mutex_);
        seg_downloaded_ = v;
    }

    net::awaitable<boost::system::error_code>
    progress_tracker::wait_if_paused(std::atomic<bool> const& cancelled)
    {
        while (paused_.load(std::memory_order_relaxed))
        {
            set_state(downloader::state::paused, {});
            co_await pause_event_.wait();
            if (cancelled.load(std::memory_order_relaxed))
            {
                co_return boost::asio::error::operation_aborted;
            }
        }
        set_state(downloader::state::downloading, {});
        co_return boost::system::error_code {};
    }

    void
    progress_tracker::set_state(downloader::state st, boost::system::error_code ec)
    {
        state_.store(st);
        auto cb = state_cb_.load();
        if (cb && *cb)
        {
            (*cb)(st, ec);
        }
    }

    downloader::state
    progress_tracker::state() const
    {
        return state_.load();
    }

    void
    progress_tracker::pause()
    {
        paused_.store(true, std::memory_order_relaxed);
    }

    void
    progress_tracker::resume()
    {
        paused_.store(false, std::memory_order_relaxed);
        pause_event_.notify_all();
    }

    bool
    progress_tracker::is_paused() const
    {
        return paused_.load(std::memory_order_relaxed);
    }

    void
    progress_tracker::notify_all()
    {
        pause_event_.notify_all();
    }

    std::uint64_t
    progress_tracker::total_bytes() const
    {
        std::lock_guard lk(mutex_);
        return total_bytes_;
    }

    int
    progress_tracker::total_segments() const
    {
        std::lock_guard lk(mutex_);
        return total_segments_;
    }

    void
    progress_tracker::set_active_segments(int n)
    {
        std::lock_guard lk(mutex_);
        active_segments_ = n;
    }

    std::vector<std::uint64_t>
    progress_tracker::seg_downloaded() const
    {
        std::lock_guard lk(mutex_);
        return seg_downloaded_;
    }

    void
    progress_tracker::finish()
    {
        auto cb = progress_cb_.load();
        if (!cb || !*cb)
        {
            return;
        }
        downloader::progress_info info;
        {
            std::lock_guard lk(mutex_);
            auto final_sz = total_bytes_ > 0 ? total_bytes_ : downloaded_bytes_;
            info = downloader::progress_info { final_sz, final_sz, 0, std::chrono::seconds(0), 0, 1 };
        }
        (*cb)(info);
    }

    void
    progress_tracker::finish_with_bytes(std::uint64_t sz)
    {
        auto cb = progress_cb_.load();
        if (!cb || !*cb)
        {
            return;
        }
        (*cb)(downloader::progress_info { sz, sz, 0, std::chrono::seconds(0), 0, 1 });
    }

    void
    progress_tracker::notify_initial(std::uint64_t total, std::uint64_t downloaded, int segments)
    {
        auto cb = progress_cb_.load();
        if (!cb || !*cb)
        {
            return;
        }
        (*cb)(downloader::progress_info { total, downloaded, 0, std::chrono::seconds(0), segments, segments });
    }

} // namespace httplib::client
