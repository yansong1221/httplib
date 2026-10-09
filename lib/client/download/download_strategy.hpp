#pragma once
#include "disk_writer.h"
#include "progress_tracker.hpp"
#include "request_sender.hpp"
#include "state_store.hpp"
#include "httplib/client/downloader.hpp"
#include "httplib/headers.hpp"
#include "httplib/url/url.hpp"
#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace httplib::client
{

    /// Runs a single- or multi-segment download against an already-probed URL.
    /// Owns the retry/backoff loops and range bookkeeping; delegates connection
    /// work to a `send_fn` (so it stays free of pool/sender wiring) and reports
    /// response headers through sinks.
    class download_strategy
    {
      public:
        /// Sends one logical request and follows redirects. Supplied by the
        /// downloader so this class does not own the request_sender.
        using send_fn = std::function<net::awaitable<request_sender::result>(
            url::url_info const&, httplib::method, httplib::headers const&)>;
        using headers_sink = std::function<void(httplib::headers const&)>;

        struct segment_task
        {
            std::uint64_t start_byte;
            std::uint64_t end_byte;
            int index;
        };

        download_strategy(disk_writer& disk,
                          progress_tracker& progress,
                          std::atomic<bool>& cancelled,
                          downloader::config cfg,
                          send_fn send,
                          headers_sink on_suggested_filename,
                          headers_sink on_resource_headers,
                          std::string state_url);

        net::awaitable<boost::system::error_code> download_single(url::url_info const& ui, fs::path const& save_path);

        net::awaitable<boost::system::error_code> download_multi(url::url_info const& ui,
                                                                 fs::path const& save_path,
                                                                 std::uint64_t content_length,
                                                                 httplib::headers const& probe_headers);

      private:
        net::awaitable<boost::system::error_code> download_segment(url::url_info const& ui,
                                                                   std::uint64_t start,
                                                                   std::uint64_t end,
                                                                   int index);

        net::awaitable<request_sender::result> send(url::url_info const& ui,
                                                    httplib::method m,
                                                    httplib::headers const& req_headers);

        void save_state(fs::path const& save_path);

        disk_writer& disk_;
        progress_tracker& progress_;
        std::atomic<bool>& cancelled_;
        downloader::config cfg_;
        send_fn send_;
        headers_sink on_suggested_filename_;
        headers_sink on_resource_headers_;
        std::string state_url_;
        std::vector<segment_task> segments_;
    };

} // namespace httplib::client
