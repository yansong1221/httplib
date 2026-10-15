#pragma once
#include "httplib/client/client_fwd.hpp"
#include "httplib/client/error.hpp"
#include "httplib/enums.hpp"
#include "httplib/headers.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::client
{

    class HTTPLIB_API downloader
    {
      public:
        struct progress_info
        {
            std::uint64_t total_bytes = 0;
            std::uint64_t downloaded_bytes = 0;
            std::uint64_t speed_bytes_per_sec = 0;
            std::chrono::seconds eta { 0 };
        };

        using progress_callback = std::function<void(progress_info const&)>;

        enum class state
        {
            idle,
            connecting,
            downloading,
            paused,
            completed,
            failed,
            cancelled
        };

        using state_callback = std::function<void(state st, boost::system::error_code ec)>;

        struct config
        {
            int max_retries = 3;
            /// 单个 I/O 步骤（建连、发送、一次读）的超时，兼作「多久没数据就放弃」
            /// 的停顿时限。整体下载时长不受它限制，大文件不会被总时限掐断。
            std::chrono::steady_clock::duration timeout = std::chrono::seconds(60);
            std::chrono::milliseconds acquire_timeout = std::chrono::seconds(30);
            int max_redirects = 5;
            bool resume = true;
            bool verify_ssl = true;
            std::uint64_t max_speed_bytes_per_sec = 0;
            /// Delay before a retry attempt after a transient failure. Scaled by
            /// the attempt number so repeated failures back off instead of
            /// hammering the server. `0` disables the backoff.
            std::chrono::milliseconds retry_backoff = std::chrono::milliseconds(200);
        };

      public:
        explicit downloader(net::io_context& ex, std::shared_ptr<http_client_pool> pool);
        explicit downloader(net::any_io_executor const& ex, std::shared_ptr<http_client_pool> pool);
        ~downloader();

        downloader(downloader const&) = delete;
        downloader& operator=(downloader const&) = delete;
        downloader(downloader&&) noexcept;
        downloader& operator=(downloader&&) noexcept;

        void set_config(config const& cfg);
        void set_progress_callback(progress_callback cb);
        void set_state_callback(state_callback cb);

        void set_cache(std::shared_ptr<cache> c);
        std::shared_ptr<cache> get_cache() const;
        std::shared_ptr<http_client_pool> get_http_pool() const;

        config get_config() const;

        /// Downloads \p url into `save_path`. The body is written to
        /// `<save_path>.part` and renamed to `save_path` on success; a failed or
        /// cancelled run leaves the `.part` file behind so a later call can
        /// resume from it.
        ///
        /// Permanent statuses (4xx other than 408/429) fail immediately without
        /// retrying; 408/429 and 5xx are retried up to `config::max_retries`
        /// times. When the run fails on such a status, http_status() reports
        /// which one.
        net::awaitable<boost::system::error_code> async_download(std::string_view url,
                                                                 fs::path const& save_path,
                                                                 httplib::headers const& headers = {});

        /// \copydoc async_download(std::string_view, fs::path const&, httplib::headers const&)
        std::future<boost::system::error_code> download(std::string_view url,
                                                        fs::path const& save_path,
                                                        httplib::headers const& headers = {});

        void cancel();
        void pause();
        void resume();
        bool is_paused() const;

        std::string suggested_filename() const;
        state current_state() const;

        /// The terminal HTTP status of the most recent run: `ok`/`partial_content`
        /// for a body-bearing success, the server's status otherwise (e.g.
        /// `not_found`). `status::unknown` when no response was received at all
        /// (transport failure, cancellation). Reset at the start of every run.
        httplib::status http_status() const;

      private:
        class impl;
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::client
