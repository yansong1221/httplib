#pragma once

#include "body/any_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/util/async_mutex.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include "util/logging.hpp"
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <functional>
#include <limits>
#include <spdlog/spdlog.h>

namespace httplib::client
{

    class http_client::impl
        : public detail::logger
        , public std::enable_shared_from_this<http_client::impl>
    {
      public:
        class lazy_request_impl;

        using body_setup_fn = std::function<void(http::response<body::any_body>&)>;

        impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl);

        void
        set_timeout_policy(timeout_policy const& policy)
        {
            timeout_policy_ = policy;
        }

        void
        set_timeout(std::chrono::steady_clock::duration const& duration)
        {
            timeout_ = duration;
        }

        void
        set_max_redirects(int n)
        {
            max_redirects_ = n;
        }

        void
        set_header_limit(std::uint32_t limit)
        {
            header_limit_ = limit;
        }

        void
        set_body_limit(std::uint64_t limit)
        {
            body_limit_ = limit;
        }

        void set_download_rate_limit(std::uint64_t bytes_per_second);
        void set_upload_rate_limit(std::uint64_t bytes_per_second);

      public:
        void close();
        bool is_open() const;
        bool has_active_session() const;
        bool is_alive() const;

        net::awaitable<http_client::response_result> async_send_request_lazy(http_client::request& req);

        net::awaitable<http_client::response_result> async_send_request_lazy_with_redirect(http_client::request& req);

        std::shared_ptr<lazy_request> create_lazy_request();

      private:
        friend class ::httplib::client::response::impl;

        void prepare_request(http_client::request& req);
        net::awaitable<boost::system::error_code> co_connect();

        /// Apply the stored read/write rate limits to `stream_`. Caller must hold
        /// stream_mutex_.
        void apply_rate_limits();

        void begin_io();
        void end_io();
        void finish_io();

        static bool
        is_retryable(boost::system::error_code ec)
        {
            return ec == boost::asio::error::connection_aborted || ec == boost::asio::error::connection_reset
                   || ec == http::error::end_of_stream;
        }

        template <typename Body>
        net::awaitable<boost::system::error_code>
        async_write(http::request_serializer<Body>& serializer, bool headers_only, bool retry = true)
        {
            boost::system::error_code ec;
            if (ec = co_await co_connect(); ec)
            {

                co_return ec;
            }

            bool bytes_written = false;
            serializer.split(headers_only);
            while (headers_only ? !serializer.is_header_done() : !serializer.is_done())
            {
                begin_io();
                co_await http::async_write_some(*stream_, serializer, util::net_awaitable[ec]);
                if (ec)
                {
                    if (ec != http::error::need_buffer)
                    {
                        close();
                    }
                    break;
                }
                bytes_written = true;
                end_io();
            }

            if (is_retryable(ec) && retry && !bytes_written)
            {
                close();
                get_logger()->trace("retrying request...");
                co_return co_await async_write(serializer, headers_only, false);
            }
            co_return ec;
        }
        template <typename Body>
        net::awaitable<boost::system::error_code>
        async_read(http::response_parser<Body>& parser, bool headers_only)
        {
            boost::system::error_code ec;
            parser.eager(!headers_only);
            while (headers_only ? !parser.is_header_done() : !parser.is_done())
            {
                begin_io();
                co_await http::async_read_some(*stream_, buffer_, parser, util::net_awaitable[ec]);
                if (ec)
                {
                    if (ec != http::error::need_buffer)
                    {
                        close();
                    }
                    break;
                }
                end_io();
                if (parser.is_done())
                {
                    finish_io();
                    if (!parser.keep_alive())
                    {
                        close();
                    }
                }
            }
            co_return ec;
        }

        template <typename Body>
        net::awaitable<boost::system::error_code>
        async_read_once(http::response_parser<Body>& parser)
        {
            boost::system::error_code ec;
            parser.eager(true);

            if (buffer_.size() > 0)
            {
                auto const used = parser.put(buffer_.data(), ec);
                buffer_.consume(used);
                if (ec != http::error::need_more)
                {
                    co_return normalize_read(parser, ec);
                }
            }

            begin_io();
            auto mb = buffer_.prepare(65536);
            auto const n = co_await stream_->async_read_some(mb, util::net_awaitable[ec]);
            buffer_.commit(n);
            end_io();

            if (ec == net::error::eof)
            {
                if (parser.got_some())
                {
                    ec = {};
                    parser.put_eof(ec);
                }
                else
                {
                    ec = http::error::end_of_stream;
                }
                co_return normalize_read(parser, ec);
            }
            if (ec)
            {
                co_return normalize_read(parser, ec);
            }

            auto const used = parser.put(buffer_.data(), ec);
            buffer_.consume(used);
            co_return normalize_read(parser, ec);
        }

        template <typename Body>
        boost::system::error_code
        normalize_read(http::response_parser<Body>& parser, boost::system::error_code ec)
        {
            if (ec == http::error::need_more || ec == http::error::need_buffer)
            {
                ec = {};
            }

            if (parser.is_done())
            {
                finish_io();
                if (!parser.keep_alive())
                {
                    close();
                }
            }
            else if (ec)
            {
                close();
            }
            return ec;
        }

      public:
        net::any_io_executor executor_;
        // 连接级读取互斥：临界区跨越 async_read_some 的挂起点，串行化同一连接上
        // 的并发读（header/body/streaming），避免 parser / buffer_ 数据竞争。
        util::async_mutex read_mutex_;
        tcp::resolver resolver_;
        timeout_policy timeout_policy_ = timeout_policy::overall;
        std::chrono::steady_clock::duration timeout_ = std::chrono::seconds(30);
        bool overall_timer_active_ = false;

        std::string const host_;
        std::string const host_value_;
        uint16_t const port_;
        bool const use_ssl_;
        bool verify_ssl_ = true;
        std::string ca_cert_;

        std::unique_ptr<http_stream> stream_;
        mutable std::recursive_mutex stream_mutex_;
        beast::flat_buffer buffer_;
        std::weak_ptr<lazy_request_impl> write_impl_;
        std::weak_ptr<void> read_impl_;

        int max_redirects_ = 0;

        std::uint32_t header_limit_ = 65536;
        std::uint64_t body_limit_ = 1024ULL * 1024 * 1024;
        /// Response-body (download) throughput cap in bytes/sec; 0 means unlimited.
        std::uint64_t download_rate_limit_ = 0;
        /// Request-body (upload) throughput cap in bytes/sec; 0 means unlimited.
        std::uint64_t upload_rate_limit_ = 0;
    };

} // namespace httplib::client
