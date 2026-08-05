#pragma once
#include "httplib/client/cache.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/downloader.hpp"
#include <atomic>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <mutex>
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

        struct url_info
        {
            std::string host;
            uint16_t port = 0;
            bool ssl = false;
            std::string path;
        };

        struct probe_result
        {
            std::uint64_t content_length = 0;
            http::fields headers;
        };

        struct redirect_target
        {
            std::string host;
            uint16_t port = 0;
            bool ssl = false;
            std::string path;
            bool valid = false;
        };

        struct request_result
        {
            http_client_pool::client_handle handle;
            std::shared_ptr<read_session> reader;
            http::fields headers;
            http::status status = http::status::unknown;
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

        downloader::config const& get_config() const;
        downloader::state current_state() const;

        net::awaitable<boost::system::error_code> async_download(std::string_view url,
                                                                  fs::path const& save_path,
                                                                  http::fields const& headers = {});
        void cancel();

        std::string suggested_filename() const;

      public:
        net::any_io_executor executor_;

      private:
        static url_info parse_url(std::string_view url);
        static std::uint64_t parse_content_range_total(http::fields const& headers);
        static std::string parse_content_disposition_filename(http::fields const& headers);
        static std::optional<redirect_target> parse_redirect(http::fields const& headers);

        void set_state(downloader::state st, std::string_view msg = {});
        void update_progress(std::uint64_t delta_bytes);

        void save_state(fs::path const& save_path);
        download_state load_state(fs::path const& save_path) const;
        void del_state(fs::path const& save_path) const;
        static fs::path state_path(fs::path const& save_path);

        net::awaitable<bool> check_remote_cache(url_info const& ui);
        net::awaitable<probe_result> probe_content_length(url_info const& ui);

        net::awaitable<request_result> send_request(url_info const& ui,
                                                    http::verb method,
                                                    http::fields const& req_headers = {});

        net::awaitable<boost::system::error_code> co_download_single(url_info const& ui, fs::path const& save_path);

        net::awaitable<boost::system::error_code> co_download_segment(url_info const& ui,
                                                                      std::uint64_t start,
                                                                      std::uint64_t end,
                                                                      fs::path const& part_path);

        net::awaitable<boost::system::error_code> co_download_multi_segment(url_info const& ui,
                                                                            fs::path const& save_path,
                                                                            std::uint64_t content_length,
                                                                            http::fields const& probe_headers);

        boost::system::error_code merge_parts_sync(fs::path const& save_path, int total_segments);

      private:
        downloader::config config_;

        mutable std::mutex callback_mutex_;
        downloader::progress_callback progress_cb_;
        downloader::state_callback state_cb_;

        mutable std::mutex state_mutex_;
        downloader::state state_ = downloader::state::idle;
        std::string state_msg_;

        mutable std::mutex progress_mutex_;
        std::uint64_t total_bytes_ = 0;
        std::uint64_t downloaded_bytes_ = 0;
        int active_segments_ = 0;
        int total_segments_ = 1;
        std::chrono::steady_clock::time_point progress_start_;

        std::atomic<bool> cancelled_ { false };

        std::vector<segment_task> segments_;

        std::shared_ptr<cache> cache_;
        std::shared_ptr<http_client_pool> pool_;
        http::fields custom_headers_;

        mutable std::mutex filename_mutex_;
        std::string suggested_filename_;
    };

} // namespace httplib::client
