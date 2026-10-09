#pragma once
#include "cache_manager.hpp"
#include "disk_writer.h"
#include "httplib/client/cache.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/downloader.hpp"
#include "httplib/url/url.hpp"
#include "progress_tracker.hpp"
#include <atomic>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace httplib::client
{

    class downloader::impl : public std::enable_shared_from_this<downloader::impl>
    {
      public:
        struct probe_result
        {
            std::uint64_t content_length = 0;
            httplib::headers headers;
        };

        /// Terminal response of a logical request after following redirects.
        struct send_result
        {
            http_client_pool::client_handle handle;
            client::response response;
            httplib::headers headers;
            httplib::status status = status::unknown;
            /// Set when no usable response was obtained (connection/acquire/send
            /// failure, cancellation, redirect exhaustion).
            boost::system::error_code error;
            /// Origin/target actually reached after following redirects.
            url::url_info final_ui;
        };

      public:
        impl(net::any_io_executor ex, std::shared_ptr<http_client_pool> pool);
        ~impl();

        void set_config(downloader::config const& cfg);
        void set_progress_callback(downloader::progress_callback cb);
        void set_state_callback(downloader::state_callback cb);

        void set_cache(std::shared_ptr<cache> c);
        std::shared_ptr<cache> get_cache() const;
        std::shared_ptr<http_client_pool> get_http_pool() const;

        downloader::config get_config() const;
        downloader::state current_state() const;

        net::awaitable<boost::system::error_code> async_download(std::string_view url,
                                                                 fs::path const& save_path,
                                                                 httplib::headers const& headers = {});

        std::future<boost::system::error_code>
        download(std::string_view url, fs::path const& save_path, httplib::headers const& headers = {})
        {
            return net::co_spawn(
                executor_,
                [this, self = shared_from_this(), url = std::string(url), save_path, headers]()
                    -> net::awaitable<boost::system::error_code>
                { co_return co_await async_download(url, save_path, headers); },
                net::use_future);
        }
        void cancel();
        void pause();
        void resume();
        bool is_paused() const;

        std::string suggested_filename() const;

      private:
        void store_suggested_filename(httplib::headers const& headers);
        void record_final_ui(url::url_info const& ui);
        void record_resource_headers(httplib::headers const& headers);

        /// Body of a single run. Spawned by async_download() so its I/O is bound
        /// to that run's cancellation signal. Owns its URL by value.
        net::awaitable<boost::system::error_code> run_download(std::string_view url,
                                                               fs::path const& save_path,
                                                               httplib::headers const& headers);

        net::awaitable<bool> check_remote_cache(url::url_info const& ui,
                                                cache_manager::http_meta const& meta,
                                                httplib::headers const& base_headers);
        net::awaitable<probe_result> probe_content_length(url::url_info const& ui, httplib::headers const& base_headers);

        /// Sends one logical request over a pooled connection and follows
        /// redirects, returning the terminal response. `req_headers` must already
        /// contain the fully merged per-request headers.
        net::awaitable<send_result> send_request(url::url_info const& ui,
                                                 httplib::method m,
                                                 httplib::headers req_headers);

        /// Single-stream download with resume-on-interruption. Streams the body
        /// straight to disk and retries within `cfg.max_retries`.
        net::awaitable<boost::system::error_code> download_single(url::url_info const& ui,
                                                                  fs::path const& save_path,
                                                                  downloader::config const& cfg,
                                                                  httplib::headers const& base_headers);

      private:
        net::any_io_executor executor_;
        /// Serializes all of this downloader's payload writes on one strand.
        disk_writer disk_;
        std::atomic<downloader::config> config_ { downloader::config {} };

        progress_tracker progress_;

        std::atomic<bool> cancelled_ { false };

        /// Per-run cancellation source. async_download() installs a fresh signal
        /// for each run and binds it to the run's coroutine, so cancel() aborts
        /// in-flight I/O immediately instead of waiting for a timeout. A cancel()
        /// issued while no run is active finds no signal and is a no-op, so it
        /// cannot leak into the next run.
        std::atomic<std::shared_ptr<net::cancellation_signal>> cancel_signal_ { nullptr };

        cache_manager cache_manager_;
        std::shared_ptr<http_client_pool> pool_;

        /// Captured from the download coroutine so the completed transfer's real
        /// response headers (and final origin after redirects) can be written to
        /// the cache.
        mutable std::mutex resource_mutex_;
        httplib::headers resource_headers_;
        url::url_info final_ui_;
        bool has_final_ui_ = false;

        mutable std::mutex filename_mutex_;
        std::string suggested_filename_;
    };

} // namespace httplib::client
