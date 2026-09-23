#pragma once
#include "httplib/client/cache.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/downloader.hpp"
#include "httplib/url/url.hpp"
#include "httplib/util/async_event.hpp"
#include <atomic>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::client
{

    class downloader::impl
    {
      public:
        struct segment_task
        {
            std::uint64_t start_byte;
            std::uint64_t end_byte;
            int index;
            fs::path part_path;
        };

        struct download_state
        {
            std::string url;
            std::uint64_t content_length = 0;
            int segments = 0;
            std::vector<std::uint64_t> seg_downloaded;
        };

        struct probe_result
        {
            std::uint64_t content_length = 0;
            http::fields headers;
        };

        struct request_result
        {
            http_client_pool::client_handle handle;
            client::response response;
            http::fields headers;
            http::status status = http::status::unknown;
            /// Set when no usable response was obtained (connection/acquire/
            /// send failure, cancellation, redirect exhaustion). Callers
            /// propagate this instead of collapsing every failure into a
            /// generic timeout.
            boost::system::error_code error;
            /// Origin/target actually reached after following redirects. Used as
            /// the cache identity so a URL that redirects elsewhere cannot serve
            /// stale cached content.
            url::url_info final_ui;
        };

        /// HTTP-specific cache bookkeeping. The downloader serializes this into
        /// the cache's opaque metadata blob; the cache itself never interprets
        /// it.
        struct http_meta
        {
            std::string final_url;
            std::string etag;
            std::string last_modified;
            std::string content_type;
            std::string content_disposition;
            /// Time until which the response is fresh (Cache-Control max-age).
            /// Within this window the cached body may be served without network.
            std::optional<std::chrono::system_clock::time_point> fresh_until;
            /// Cache-Control: no-cache -> must revalidate even if fresh.
            bool must_revalidate = false;
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
                                                                 http::fields const& headers = {});
        void cancel();
        void pause();
        void resume();
        bool is_paused() const;

        std::string suggested_filename() const;

      public:
        net::any_io_executor executor_;

      private:
        std::string make_cache_key(url::url_info const& ui) const;
        static std::string cache_auth_scope(http::fields const& headers);
        static bool response_is_cacheable(http::fields const& headers);
        static http_meta make_http_meta(http::fields const& response,
                                        http::fields const& probe,
                                        url::url_info const& final_ui,
                                        bool has_final_ui);
        static std::string serialize_http_meta(http_meta const& meta);
        static std::optional<http_meta> parse_http_meta(std::string_view blob);
        static std::uint64_t parse_content_range_total(http::fields const& headers);
        static std::optional<std::uint64_t> parse_content_range_start(http::fields const& headers);
        static std::string parse_content_disposition_filename(http::fields const& headers);
        static std::optional<url::url_info> parse_redirect(http::fields const& headers);

        void set_state(downloader::state st, boost::system::error_code ec);
        void update_progress(std::uint64_t delta_bytes);
        void store_suggested_filename(http::fields const& headers);
        void record_final_ui(url::url_info const& ui);
        void record_resource_headers(http::fields const& headers);

        void save_state(fs::path const& save_path);
        download_state load_state(fs::path const& save_path) const;
        void del_state(fs::path const& save_path) const;
        static fs::path state_path(fs::path const& save_path);

        net::awaitable<bool> check_remote_cache(url::url_info const& ui, http_meta const& meta);
        net::awaitable<probe_result> probe_content_length(url::url_info const& ui);

        net::awaitable<request_result> send_request(url::url_info const& ui,
                                                    http::verb method,
                                                    http::fields const& req_headers = {});

        net::awaitable<boost::system::error_code> co_wait_if_paused();

        net::awaitable<boost::system::error_code> co_download_single(url::url_info const& ui, fs::path const& save_path);

        net::awaitable<boost::system::error_code> co_download_segment(url::url_info const& ui,
                                                                      std::uint64_t start,
                                                                      std::uint64_t end,
                                                                      fs::path const& part_path);

        net::awaitable<boost::system::error_code> co_download_multi_segment(url::url_info const& ui,
                                                                            fs::path const& save_path,
                                                                            std::uint64_t content_length,
                                                                            http::fields const& probe_headers);

        boost::system::error_code merge_parts_sync(fs::path const& save_path,
                                                   int total_segments,
                                                   std::uint64_t expected_total);

      private:
        downloader::config config_;
        mutable std::mutex config_mutex_;
        /// Snapshot of `config_` captured when a download starts. Only touched by
        /// the download coroutine, so it needs no locking once the run begins.
        downloader::config active_config_;
        /// Canonical URL of the current run, persisted in the sidecar state file
        /// so a stale state from another URL is never reused.
        std::string state_url_;

        mutable std::mutex callback_mutex_;
        downloader::progress_callback progress_cb_;
        downloader::state_callback state_cb_;

        mutable std::mutex state_mutex_;
        downloader::state state_ = downloader::state::idle;

        mutable std::mutex progress_mutex_;
        std::uint64_t total_bytes_ = 0;
        std::uint64_t downloaded_bytes_ = 0;
        int active_segments_ = 0;
        int total_segments_ = 1;
        std::chrono::steady_clock::time_point progress_start_;

        /// Per-connection throughput cap for the current run (bytes/sec, 0 =
        /// unlimited). For multi-segment downloads this is the configured cap
        /// divided across the concurrent connections.
        std::uint64_t per_connection_rate_ = 0;

        std::atomic<bool> cancelled_ { false };
        std::atomic<bool> paused_ { false };
        util::async_event pause_event_;

        std::vector<segment_task> segments_;

        std::shared_ptr<cache> cache_;
        std::shared_ptr<http_client_pool> pool_;
        http::fields custom_headers_;
        /// Hash of credential-bearing request headers folded into the cache key
        /// so two callers with different credentials never share an entry.
        std::string auth_scope_;

        /// Captured from the concurrent segment/GET coroutines so the completed
        /// transfer's real response headers (and final origin after redirects)
        /// can be written to the cache.
        mutable std::mutex resource_mutex_;
        http::fields resource_headers_;
        url::url_info final_ui_;
        bool has_final_ui_ = false;

        mutable std::mutex filename_mutex_;
        std::string suggested_filename_;
    };

} // namespace httplib::client
