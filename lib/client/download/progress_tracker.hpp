#pragma once
#include "httplib/client/downloader.hpp"
#include "httplib/util/async_event.hpp"
#include <atomic>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace httplib::client
{

    class progress_tracker
    {
      public:
        explicit progress_tracker(net::any_io_executor ex);

        void set_progress_callback(downloader::progress_callback cb);
        void set_state_callback(downloader::state_callback cb);

        void start(std::uint64_t total_bytes, int total_segments);
        void set_downloaded(std::uint64_t n);
        void update(std::uint64_t delta_bytes);
        void record_segment_bytes(int index, std::uint64_t n);
        std::uint64_t segment_bytes(int index) const;
        void reset_segment(int index);
        void set_segment_bytes(int index, std::uint64_t n);
        void set_all_segment_bytes(std::vector<std::uint64_t> const& v);

        net::awaitable<boost::system::error_code> wait_if_paused(std::atomic<bool> const& cancelled);

        void set_state(downloader::state st, boost::system::error_code ec);
        downloader::state state() const;

        void pause();
        void resume();
        bool is_paused() const;
        void notify_all();

        std::uint64_t total_bytes() const;
        int total_segments() const;
        void set_active_segments(int n);
        std::vector<std::uint64_t> seg_downloaded() const;
        void finish();
        void finish_with_bytes(std::uint64_t sz);
        void notify_initial(std::uint64_t total, std::uint64_t downloaded, int segments);

      private:
        net::any_io_executor executor_;
        util::async_event pause_event_;

        std::atomic<std::shared_ptr<downloader::progress_callback>> progress_cb_;
        std::atomic<std::shared_ptr<downloader::state_callback>> state_cb_;
        std::atomic<downloader::state> state_;

        mutable std::mutex mutex_;
        std::uint64_t total_bytes_;
        std::uint64_t downloaded_bytes_;
        int active_segments_;
        int total_segments_;
        std::chrono::steady_clock::time_point progress_start_;
        std::vector<std::uint64_t> seg_downloaded_;

        std::atomic<bool> paused_;
    };

} // namespace httplib::client
