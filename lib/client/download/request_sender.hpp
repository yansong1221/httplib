#pragma once
#include "httplib/client/client_pool.hpp"
#include "httplib/client/downloader.hpp"
#include "httplib/headers.hpp"
#include "httplib/url/url.hpp"
#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <memory>

namespace httplib::client
{

    /// Sends a single logical request over a pooled connection and follows
    /// redirects, returning the terminal response. Owns no run state: the
    /// cancellation flag is passed in per call so the sender can be reused.
    class request_sender
    {
      public:
        struct config
        {
            std::chrono::milliseconds acquire_timeout;
            std::chrono::steady_clock::duration timeout;
            bool verify_ssl = true;
            int max_redirects = 5;
        };

        struct result
        {
            http_client_pool::client_handle handle;
            client::response response;
            httplib::headers headers;
            httplib::status status = status::unknown;
            /// Set when no usable response was obtained (connection/acquire/
            /// send failure, cancellation, redirect exhaustion).
            boost::system::error_code error;
            /// Origin/target actually reached after following redirects.
            url::url_info final_ui;
        };

        request_sender(std::shared_ptr<http_client_pool> pool,
                       httplib::headers base_headers,
                       config cfg);

        net::awaitable<result> send(url::url_info const& ui,
                                    httplib::method m,
                                    httplib::headers const& extra_headers,
                                    std::atomic<bool> const& cancelled,
                                    std::uint64_t per_connection_rate);

      private:
        std::shared_ptr<http_client_pool> pool_;
        httplib::headers base_headers_;
        config cfg_;
    };

} // namespace httplib::client
