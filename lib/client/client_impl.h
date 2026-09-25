#pragma once

#include "body/source.hpp"
#include "httplib/client/client.hpp"
#include "httplib/util/async_mutex.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include "util/logging.hpp"
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <functional>
#include <future>
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

        impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, httplib::url::scheme s);

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
        std::future<void> close();
        net::awaitable<void> async_close();

        bool is_open() const;
        bool has_active_session() const;

        net::awaitable<bool> async_is_alive() const;

        net::awaitable<http_client::response_result> async_send_request_lazy_with_redirect(request& req);

        std::shared_ptr<lazy_request> create_lazy_request();

        net::awaitable<http_client::response_result> read_response_lazy(http::verb method);

        // body_writer 的连接写入原语：
        //   write_header 只写头；write_some 单步写；write 写到 need_buffer/完成。
        template <typename Serializer>
        net::awaitable<void>
        write_header(Serializer& sr, boost::system::error_code& ec)
        {
            co_await async_write(sr, true, ec);
        }
        template <typename Serializer>
        net::awaitable<void>
        write_some(Serializer& sr, boost::system::error_code& ec)
        {
            co_await async_write_some(sr, ec);
        }
        template <typename Serializer>
        net::awaitable<void>
        write(Serializer& sr, boost::system::error_code& ec)
        {
            co_await async_write(sr, false, ec);
            if (ec == http::error::need_buffer)
            {
                ec = {};
            }
        }

      private:
        friend class ::httplib::client::response::impl;

        void prepare_request(request& req);
        net::awaitable<void> co_connect(boost::system::error_code& ec);
        net::awaitable<http_client::response_result> async_send_request_lazy(request& req);
        net::awaitable<void> write_request(request& req, boost::system::error_code& ec);

        /// Apply the stored read/write rate limits to the current stream（`stream_`）。
        /// 可在任意线程调用：内部把 rate_policy 的修改投递到 strand 上执行，与 Beast
        /// 绑定在同一 strand 的限速记账串行化，避免与在途读写并发改限速。
        void apply_rate_limits() const;

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

        // NOTE: async_write / async_read / async_read_some 是无锁读写原语，串行化
        // 完全依赖 strand（strand_）：每个入口先投递（co_spawn）到 strand，底层 socket 操作
        // 期间不持有任何 mutex。socket 每次操作按 `stream_->load()` 快照使用，因此并发
        // async_close() 置空 stream_ 不会造成空指针解引用，只会让在途操作以错误码返回。
        template <typename Body>
        net::awaitable<void>
        async_write(http::request_serializer<Body>& serializer, bool headers_only, boost::system::error_code& ec)
        {
            co_return co_await net::co_spawn(
                strand_,
                [&]() -> net::awaitable<void>
                {
                    bool header_done = serializer.is_header_done();
                    if (!header_done)
                    {
                        co_await co_connect(ec);
                        if (ec)
                        {
                            co_return;
                        }
                    }
                    bool retry = !header_done;

                    serializer.split(headers_only);
                    while (headers_only ? !serializer.is_header_done() : !serializer.is_done())
                    {
                        co_await async_write_some(serializer, ec);
                        if (ec)
                        {
                            if (is_retryable(ec) && retry)
                            {
                                ec = {};
                                co_await async_close();
                                get_logger()->trace("retrying request...");
                                co_await co_connect(ec);
                                if (ec)
                                {
                                    co_return;
                                }
                            }
                            else
                            {
                                break;
                            }
                        }
                        retry = false;
                    }
                },
                net::use_awaitable);
        }
        template <typename Body>
        net::awaitable<void>
        async_write_some(http::request_serializer<Body>& serializer, boost::system::error_code& ec)
        {
            co_return co_await net::co_spawn(
                strand_,
                [&]() -> net::awaitable<void>
                {
                    auto s = stream_.load();
                    if (!s)
                    {
                        ec = net::error::make_error_code(net::error::not_connected);
                        co_return;
                    }

                    begin_io();
                    co_await http::async_write_some(*s, serializer, util::net_awaitable[ec]);
                    if (ec)
                    {
                        if (ec != http::error::need_buffer)
                        {
                            co_await async_close();
                        }
                    }
                    end_io();
                },
                net::use_awaitable);
        }

        template <typename Body>
        net::awaitable<void>
        async_read(http::response_parser<Body>& parser, bool headers_only, boost::system::error_code& ec)
        {
            co_return co_await net::co_spawn(
                strand_,
                [&]() -> net::awaitable<void>
                {
                    while (headers_only ? !parser.is_header_done() : !parser.is_done())
                    {
                        co_await async_read_some(parser, ec);
                        if (ec)
                        {
                            break;
                        }
                    }
                },
                net::use_awaitable);
        }
        template <typename Body>
        net::awaitable<void>
        async_read_some(http::response_parser<Body>& parser, boost::system::error_code& ec)
        {
            co_return co_await net::co_spawn(
                strand_,
                [&]() -> net::awaitable<void>
                {
                    auto s = stream_.load();
                    if (!s)
                    {
                        ec = net::error::make_error_code(net::error::not_connected);
                        co_return;
                    }
                    begin_io();
                    co_await http::async_read_some(*s, buffer_, parser, util::net_awaitable[ec]);
                    if (ec)
                    {
                        if (ec != http::error::need_buffer)
                        {
                            co_await async_close();
                        }
                    }
                    end_io();
                    if (parser.is_done())
                    {
                        finish_io();
                        if (!parser.keep_alive())
                        {
                            co_await async_close();
                        }
                    }
                },
                net::use_awaitable);
        }

        net::any_io_executor
        get_executor() const noexcept
        {
            return strand_;
        }

      public:
        net::strand<net::any_io_executor> strand_;

        tcp::resolver resolver_;
        std::atomic<timeout_policy> timeout_policy_ { timeout_policy::overall };
        std::atomic<std::chrono::steady_clock::duration> timeout_ { std::chrono::seconds(30) };
        std::atomic<bool> overall_timer_active_ { false };

        std::string const host_;
        std::string const host_value_;
        uint16_t const port_;
        httplib::url::scheme const scheme_;
        std::atomic<bool> verify_ssl_ { true };
        /// 原子快照：set_ca_cert / copy_settings_from 写，co_connect 读，跨线程安全。
        std::atomic<std::shared_ptr<std::string const>> ca_cert_ { nullptr };

        std::atomic<std::shared_ptr<http_stream>> stream_;
        mutable std::recursive_mutex stream_mutex_;
        /// 仅由 strand（strand_）串行访问：所有基于 Beast parser 的读取都投递到
        /// strand 后共享该缓冲。
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
