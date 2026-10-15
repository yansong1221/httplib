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

    /**
     * \brief 下载器：把一个 URL 下载到本地文件。
     * \details 响应体先写入 `<save_path>.part`，成功后原子改名为 `save_path`；失败或
     * 取消则保留 `.part`，供后续调用断点续传（见 \ref config 的 `resume`）。重定向、
     * 条件请求（ETag / Last-Modified）与响应缓存均由本类内部处理。
     *
     * \par 单次运行约束
     * 一个 downloader 实例同一时刻只驱动 **一个** 传输。它内部只保留一份运行期状态：
     * 输出文件的写出流、进度计数、\ref state / \ref http_status / \ref
     * suggested_filename，以及取消源。若在同一实例上并发调用 \ref async_download 或
     * \ref download，这些状态会互相覆盖，典型后果是数据被写进另一个文件、
     * `not_initialized`，以及 \ref cancel / \ref pause / \ref resume 作用到错误的
     * 运行上。该约束未做运行时防护。
     *
     * \par 并发下载
     * 需要并发时，请 **为每个传输各建一个 downloader**，并让它们共享同一个
     * \ref http_client_pool（连接池本就设计为多方共用）：
     * \code
     * auto pool = std::make_shared<httplib::client::http_client_pool>(
     *     ioc.get_executor(), httplib::client::pool_params {});
     * // 每个并发任务：
     * httplib::client::downloader dl(ioc, pool);
     * dl.set_config(cfg);
     * auto ec = co_await dl.async_download(url, save_path);
     * \endcode
     *
     * \par 生命周期
     * 不可拷贝、可移动；析构会 \ref cancel 尚未完成的运行。
     */
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
        ///
        /// At most one run may be in flight per downloader. To download
        /// concurrently, use one downloader per run and share the pool; see the
        /// class documentation.
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
