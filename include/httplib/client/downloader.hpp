#pragma once
#include "httplib/client/client_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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
            int active_segments = 0;
            int total_segments = 0;
        };

        using progress_callback = std::function<void(progress_info const&)>;

        enum class state
        {
            idle,
            connecting,
            downloading,
            merging,
            completed,
            failed,
            cancelled
        };

        using state_callback = std::function<void(state st, std::string_view msg)>;

        struct config
        {
            int segments = 4;
            int max_retries = 3;
            std::chrono::steady_clock::duration timeout = std::chrono::seconds(60);
            int max_redirects = 5;
            bool resume = true;
            bool verify_ssl = true;
            std::uint64_t max_speed_bytes_per_sec = 0;
            bool save_state = true;
        };

      public:
        explicit downloader(net::io_context& ex, std::shared_ptr<http_client_pool> pool);
        explicit downloader(net::any_io_executor const& ex, std::shared_ptr<http_client_pool> pool);
        ~downloader();

        downloader(downloader const&) = delete;
        downloader& operator=(downloader const&) = delete;
        downloader(downloader&&) = default;
        downloader& operator=(downloader&&) = default;

        void set_config(config const& cfg);
        void set_progress_callback(progress_callback cb);
        void set_state_callback(state_callback cb);

        void set_cache(std::shared_ptr<cache> c);
        std::shared_ptr<cache> get_cache() const;
        std::shared_ptr<http_client_pool> get_http_pool() const;

        config const& get_config() const;

        net::awaitable<boost::system::error_code> async_download(std::string_view url,
                                                                 fs::path const& save_path,
                                                                 http::fields const& headers = {});

        boost::system::error_code download(std::string_view url,
                                           fs::path const& save_path,
                                           http::fields const& headers = {});

        void cancel();

        std::string suggested_filename() const;
        state current_state() const;

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::client
