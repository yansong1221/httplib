#pragma once
#include "cache_manager.hpp"
#include "disk_writer.h"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/downloader.hpp"
#include "httplib/url/url.hpp"
#include "progress_tracker.hpp"
#include <atomic>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
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
        /// Terminal response of a logical request after following redirects: the
        /// pooled connection that owns its (lazy) body, the response itself, the
        /// URL that actually served the body, and the cache entry whose
        /// validators were attached to the final request (empty when the final
        /// URL has no cache entry). Absence in the enclosing
        /// `boost::system::result` means no usable response was obtained
        /// (connection/acquire/send failure, cancellation, redirect exhaustion).
        struct send_result
        {
            http_client_pool::client_handle handle;
            client::response response;
            url::url_info final_ui;
            std::optional<cache::entry> cache_entry;
        };

        /// Success payload of a single-stream download: the response headers
        /// needed for caching and the URL that served them (the response-cache
        /// key). `from_cache` marks a 304 that was served by copying the cached
        /// body — the run must not re-put the cache.
        struct download_payload
        {
            httplib::headers headers;
            url::url_info final_ui;
            bool from_cache = false;
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

        /// Body of a single run. Spawned by async_download() so its I/O is bound
        /// to that run's cancellation signal. Owns its URL by value.
        net::awaitable<boost::system::error_code> run_download(std::string_view url,
                                                               fs::path const& save_path,
                                                               httplib::headers const& headers);

        /// Tries to complete the run from the response cache without any network
        /// round-trip, copying the cached body to `save_path`. Only fresh entries
        /// (unexpired, non-no-cache) qualify; stale entries fall through to a
        /// conditional GET in download_single(). `cached` is the resolved entry
        /// fetched by the caller. Returns true when the cache satisfied the
        /// request.
        net::awaitable<bool> try_serve_from_cache(std::optional<cache_manager::cached_entry> const& cached,
                                                  fs::path const& save_path);

        /// Sends one logical request over a pooled connection and follows
        /// redirects, returning the terminal response. Before each hop the
        /// response cache is consulted for that hop's URL and the cached
        /// validators are attached — so a redirecting origin never receives a
        /// different origin's If-None-Match / If-Modified-Since. `req_headers`
        /// must already contain the fully merged per-request headers.
        net::awaitable<boost::system::result<send_result>> send_request(url::url_info const& ui,
                                                                        httplib::method m,
                                                                        httplib::headers req_headers);

        /// Single-stream download with resume-on-interruption. Streams the body
        /// straight to disk and retries within `cfg.max_retries`. When the
        /// conditional GET returns 304, the cached body for the final URL is
        /// copied to `save_path` and the payload is returned with
        /// `from_cache = true`.
        net::awaitable<boost::system::result<download_payload>> download_single(url::url_info const& ui,
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

        mutable std::mutex filename_mutex_;
        std::string suggested_filename_;
    };

} // namespace httplib::client
