#pragma once

#include "httplib/client/client.hpp"
#include "httplib/streaming/chunk_reader.hpp"
#include "httplib/streaming/ndjson_reader.hpp"
#include "httplib/streaming/sse_reader.hpp"
#include "stream/http_stream.hpp"
#include <exception>
#include <functional>
#include <spdlog/spdlog.h>

namespace httplib::client {

class http_client::impl : public std::enable_shared_from_this<http_client::impl>
{
public:
    class relay_impl;

    using body_setup_fn = std::function<void(http_client::response&)>;

    impl(const net::any_io_executor& ex, std::string_view host, uint16_t port, bool ssl);
    ~impl();

    void set_timeout_policy(const timeout_policy& policy) { timeout_policy_ = policy; }

    void set_timeout(const std::chrono::steady_clock::duration& duration) { timeout_ = duration; }

    http_client::request make_http_request(http::verb method,
                                           std::string_view target,
                                           const http::fields& headers) const;

    void set_chunked_read_handler(chunked_read_handler_type&& handler);

    void set_sse_read_handler(sse_read_handler_type&& handler);
    void set_ndjson_read_handler(ndjson_read_handler_type&& handler);

    void set_max_redirects(int n) { max_redirects_ = n; }

public:
    void close();
    bool is_open() const;

    std::shared_ptr<spdlog::logger> logger() const;
    void set_logger(std::shared_ptr<spdlog::logger> logger);

    net::awaitable<http_client::response_result>
    async_send_request(http_client::request& req,
                       const body_setup_fn& body_setup,
                       const chunked_write_handler_type& chunked_write_handler) noexcept;

    net::awaitable<http_client::response_result>
    async_send_request_with_redirect(http_client::request& req,
                                     const body_setup_fn& body_setup,
                                     chunked_write_handler_type body_write);


    net::awaitable<http_client::response_result> async_download(http_client::request& req,
                                                                const fs::path& save_path);

    net::awaitable<std::shared_ptr<relay_session>> co_begin_relay(http::verb method,
                                                                  std::string_view target,
                                                                  const http::fields& headers,
                                                                  bool retry = true);


private:
    net::awaitable<void> co_connect();
    net::awaitable<http_client::response> co_read_response(const body_setup_fn& body_setup = {},
                                                           bool is_head                    = false);

    void expires_after(bool first = false);

    static bool is_retryable(boost::system::error_code ec)
    {
        return ec == boost::asio::error::connection_aborted ||
               ec == boost::asio::error::connection_reset || ec == http::error::end_of_stream;
    }

    boost::system::error_code handle_exception(std::exception_ptr eptr)
    {
        boost::system::error_code ec;
        try {
            std::rethrow_exception(eptr);
        }
        catch (const boost::system::system_error& error) {
            ec = error.code();
            logger()->warn(
                "{} {} {}: {}", host_, std::to_string(port_), error.what(), ec.message());
        }
        catch (const std::exception& e) {
            ec = boost::system::errc::make_error_code(boost::system::errc::protocol_error);
            logger()->warn("{} {}: {}", host_, std::to_string(port_), e.what());
        }
        catch (...) {
            ec = boost::system::errc::make_error_code(boost::system::errc::protocol_error);
            logger()->warn("{} {}: unknown exception", host_, std::to_string(port_));
        }
        close();
        return ec;
    }

    template<typename Body>
    net::awaitable<boost::system::error_code>
    async_write(http::request_serializer<Body>& serializer, bool headers_only, bool retry = true)
    {
        boost::system::error_code ec;
        try {
            co_await co_connect();

            serializer.split(headers_only);
            while (headers_only ? !serializer.is_header_done() : !serializer.is_done()) {
                expires_after();
                co_await http::async_write_some(*stream_, serializer, boost::asio::use_awaitable);
            }
            co_return ec;
        }
        catch (...) {
            ec = handle_exception(std::current_exception());
        }

        if (is_retryable(ec) && retry) {
            logger()->trace("retrying request...");
            co_return co_await async_write(serializer, headers_only, false);
        }
        co_return ec;
    }

public:
    net::any_io_executor executor_;
    tcp::resolver resolver_;
    timeout_policy timeout_policy_               = timeout_policy::overall;
    std::chrono::steady_clock::duration timeout_ = std::chrono::seconds(30);
    std::string host_;
    std::string host_value_;
    uint16_t port_ = 0;
    bool use_ssl_  = false;

    std::unique_ptr<http_stream> stream_;
    mutable std::recursive_mutex stream_mutex_;
    beast::flat_buffer buffer_;


    chunked_read_handler_type chunked_read_handler_;
    sse_read_handler_type sse_read_handler_;
    ndjson_read_handler_type ndjson_read_handler_;
    int max_redirects_ = 0;

    std::shared_ptr<spdlog::logger> default_logger_;
    std::shared_ptr<spdlog::logger> custom_logger_;
};

} // namespace httplib::client
