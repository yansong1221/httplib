#pragma once

#include "httplib/client/client.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include <boost/beast/http/read.hpp>
#include <functional>
#include <spdlog/spdlog.h>

namespace httplib::client
{

    class http_client::impl
    {
      public:
        class read_session_impl;
        class write_session_impl;

        using body_setup_fn = std::function<void(http_client::response&)>;

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

        http_client::request make_http_request(http::verb method,
                                               std::string_view target,
                                               http::fields const& headers) const;

        void
        set_max_redirects(int n)
        {
            max_redirects_ = n;
        }

      public:
        void close();
        bool is_open() const;
        bool has_active_session() const;

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        net::awaitable<http_client::response_result> async_send_request(http_client::request& req,
                                                                        body_setup_fn const& body_setup) noexcept;

        net::awaitable<http_client::response_result> async_send_request_with_redirect(http_client::request& req,
                                                                                      body_setup_fn const& body_setup);

        net::awaitable<http_client::response_result> async_download(http_client::request& req,
                                                                    fs::path const& save_path);

        std::shared_ptr<write_session> create_writer();
        std::shared_ptr<read_session> create_reader();

      private:
        net::awaitable<boost::system::error_code> co_connect();
        net::awaitable<http_client::response_result> co_read_response(body_setup_fn const& body_setup = {},
                                                                      bool is_head = false);

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
                logger()->warn("connect [{}:{}] error {}", host_, std::to_string(port_), ec.message());
                co_return ec;
            }

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
                end_io();
            }

            if (is_retryable(ec) && retry)
            {
                close();
                logger()->trace("retrying request...");
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
            }
            co_return ec;
        }

      public:
        net::any_io_executor executor_;
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
        std::weak_ptr<write_session_impl> write_impl_;
        std::weak_ptr<read_session_impl> read_impl_;

        int max_redirects_ = 0;

        std::shared_ptr<spdlog::logger> default_logger_;
        std::shared_ptr<spdlog::logger> custom_logger_;
    };

} // namespace httplib::client
