#pragma once

#include "httplib/client/client.hpp"
#include "httplib/streaming/chunk_reader.hpp"
#include "httplib/streaming/ndjson_reader.hpp"
#include "httplib/streaming/sse_reader.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include <boost/beast/http/read.hpp>
#include <functional>
#include <spdlog/spdlog.h>

namespace httplib::client {

class http_client::impl
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

    relay_session& session();


private:
    [[nodiscard]] net::awaitable<boost::system::error_code> co_connect();
    net::awaitable<http_client::response_result> co_read_response(const body_setup_fn& body_setup = {},
                                                                  bool is_head                    = false);


    void begin_io(bool first = false);
    void end_io();
    void finish_io();

    static bool is_retryable(boost::system::error_code ec)
    {
        return ec == boost::asio::error::connection_aborted ||
               ec == boost::asio::error::connection_reset || ec == http::error::end_of_stream;
    }

    template<typename Body>
    net::awaitable<boost::system::error_code>
    async_write(http::request_serializer<Body>& serializer, bool headers_only, bool retry = true)
    {
        boost::system::error_code ec;
        ec = co_await co_connect();
        if (ec) {
            logger()->warn("connect [{}:{}] error {}", host_, std::to_string(port_), ec.message());
            co_return ec;
        }

        serializer.split(headers_only);
        while (headers_only ? !serializer.is_header_done() : !serializer.is_done()) {
            begin_io();
            co_await http::async_write_some(*stream_, serializer, util::net_awaitable[ec]);
            if (ec)
                break;
            end_io();
        }
        if (ec && ec != http::error::need_buffer) {
            close();
        }
        co_return ec;


        if (is_retryable(ec) && retry) {
            logger()->trace("retrying request...");
            co_return co_await async_write(serializer, headers_only, false);
        }
        co_return ec;
    }
    template<typename Body>
    net::awaitable<boost::system::error_code> async_read(http::response_parser<Body>& parser,
                                                         bool headers_only)
    {
        boost::system::error_code ec;
        parser.eager(!headers_only);
        while (headers_only ? !parser.is_header_done() : !parser.is_done()) {
            begin_io();
            co_await http::async_read_some(*stream_, buffer_, parser, util::net_awaitable[ec]);
            if (ec)
                break;
            end_io();
        }
        if (ec && ec != http::error::need_buffer) {
            close();
        }
        co_return ec;
    }

public:
    net::any_io_executor executor_;
    tcp::resolver resolver_;
    timeout_policy timeout_policy_               = timeout_policy::overall;
    std::chrono::steady_clock::duration timeout_ = std::chrono::seconds(30);

    const std::string host_;
    const std::string host_value_;
    const uint16_t port_;
    const bool use_ssl_;

    std::unique_ptr<http_stream> stream_;
    mutable std::recursive_mutex stream_mutex_;
    beast::flat_buffer buffer_;
    std::unique_ptr<relay_impl> relay_;


    chunked_read_handler_type chunked_read_handler_;
    sse_read_handler_type sse_read_handler_;
    ndjson_read_handler_type ndjson_read_handler_;
    int max_redirects_ = 0;

    std::shared_ptr<spdlog::logger> default_logger_;
    std::shared_ptr<spdlog::logger> custom_logger_;
};

} // namespace httplib::client
