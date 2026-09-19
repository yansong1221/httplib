#pragma once

#include "body/any_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/util/async_mutex.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include "util/logging.hpp"
#include <atomic>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
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
            timeout_policy_.store(policy);
        }

        void
        set_timeout(std::chrono::steady_clock::duration const& duration)
        {
            timeout_.store(duration);
        }

        void
        set_max_redirects(int n)
        {
            max_redirects_.store(n);
        }

        void
        set_header_limit(std::uint32_t limit)
        {
            header_limit_.store(limit);
        }

        void
        set_body_limit(std::uint64_t limit)
        {
            body_limit_.store(limit);
        }

        void
        set_verify_ssl(bool verify)
        {
            verify_ssl_.store(verify);
        }

        void set_ca_cert(std::string_view cert);

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

        net::awaitable<http_client::response_result> read_response_lazy(http::verb method);

      private:
        friend class ::httplib::client::response::impl;

        void prepare_request(http_client::request& req);
        net::awaitable<void> co_connect(boost::system::error_code& ec);

        /// Apply the stored read/write rate limits to `stream_`. Caller must hold
        /// stream_mutex_.
        void apply_rate_limits(std::shared_ptr<http_stream> s) const;

        /// Copy the per-connection policy (timeout / limits / SSL / ca cert / logger)
        /// from `other` onto `*this`. Used when a redirect spawns a fresh impl.
        void copy_settings_from(impl const& other);

        void begin_io();
        void end_io();
        void finish_io();

        static bool
        is_retryable(boost::system::error_code ec)
        {
            return ec == boost::asio::error::connection_aborted || ec == boost::asio::error::connection_reset
                   || ec == http::error::end_of_stream;
        }

        // NOTE: async_write / async_read / async_read_some are lock-free primitives.
        // The caller must hold write_mutex_ (writes) / read_mutex_ (reads) for as long
        // as the operation is in flight. The socket is snapshotted per operation so a
        // concurrent close() can never turn `*stream_` into a null dereference.
        template <typename Body>
        net::awaitable<void>
        async_write(http::request_serializer<Body>& serializer,
                    bool headers_only,
                    bool retry,
                    boost::system::error_code& ec)
        {
            if (!serializer.is_header_done())
            {
                co_await co_connect(ec);
                if (ec)
                {
                    co_return;
                }
            }

            auto s = stream_.load();
            if (!s)
            {
                ec = net::error::make_error_code(net::error::not_connected);
                co_return;
            }

            bool bytes_written = false;
            serializer.split(headers_only);
            while (headers_only ? !serializer.is_header_done() : !serializer.is_done())
            {
                begin_io();
                co_await http::async_write_some(*s, serializer, util::net_awaitable[ec]);
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
                ec = {};
                close();
                get_logger()->trace("retrying request...");
                co_return co_await async_write(serializer, headers_only, false, ec);
            }
        }
        template <typename Body>
        net::awaitable<boost::system::error_code>
        async_read(http::response_parser<Body>& parser, bool headers_only)
        {
            boost::system::error_code ec;
            while (headers_only ? !parser.is_header_done() : !parser.is_done())
            {
                if (ec = co_await async_read_some(parser); ec)
                {
                    break;
                }
            }
            co_return ec;
        }
        template <typename Body>
        net::awaitable<boost::system::error_code>
        async_read_some(http::response_parser<Body>& parser)
        {
            boost::system::error_code ec;
            auto s = stream_.load();
            if (!s)
            {
                co_return net::error::make_error_code(net::error::not_connected);
            }
            parser.eager(false);
            begin_io();
            co_await http::async_read_some(*s, buffer_, parser, util::net_awaitable[ec]);
            if (ec)
            {
                if (ec != http::error::need_buffer)
                {
                    close();
                }
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
            co_return ec;
        }

      public:
        net::any_io_executor executor_;
        // resolver 绑定到独立 strand：串行化 async_resolve 与 close() 投递的
        // cancel，避免二者跨线程并发访问同一个 resolver。
        net::any_io_executor resolver_executor_;
        tcp::resolver resolver_;
        std::atomic<timeout_policy> timeout_policy_ { timeout_policy::overall };
        std::atomic<std::chrono::steady_clock::duration> timeout_ { std::chrono::seconds(30) };
        std::atomic<bool> overall_timer_active_ { false };

        std::string const host_;
        std::string const host_value_;
        uint16_t const port_;
        bool const use_ssl_;
        std::atomic<bool> verify_ssl_ { true };
        /// 仅由 stream_mutex_ 保护（std::string 非原子，co_connect 读取前需持锁）。
        std::string ca_cert_;

        std::atomic<std::shared_ptr<http_stream>> stream_;
        mutable std::recursive_mutex stream_mutex_;
        /// 仅由 read_mutex_ 保护：所有基于 Beast parser 的读取共享该缓冲。
        beast::flat_buffer buffer_;
        /// 仅由 stream_mutex_ 保护。
        std::weak_ptr<lazy_request_impl> write_impl_;
        /// 仅由 stream_mutex_ 保护。
        std::weak_ptr<void> read_impl_;

        std::atomic<int> max_redirects_ { 0 };

        std::atomic<std::uint32_t> header_limit_ { 65536 };
        std::atomic<std::uint64_t> body_limit_ { 1024ULL * 1024 * 1024 };
        /// Response-body (download) throughput cap in bytes/sec; 0 means unlimited.
        std::atomic<std::uint64_t> download_rate_limit_ { 0 };
        /// Request-body (upload) throughput cap in bytes/sec; 0 means unlimited.
        std::atomic<std::uint64_t> upload_rate_limit_ { 0 };
    };

} // namespace httplib::client
