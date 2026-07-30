#pragma once

#include "httplib/streaming/chunk_reader.hpp"
#include "httplib/client/client.hpp"
#include "httplib/streaming/ndjson_reader.hpp"
#include "httplib/streaming/sse_reader.hpp"
#include "stream/http_stream.hpp"
#include <functional>
#include <spdlog/spdlog.h>

namespace httplib::client {

class http_client::impl
{
public:
    using body_setup_fn = std::function<void(http_client::response&)>;

    impl(const net::any_io_executor& ex, std::string_view host, uint16_t port, bool ssl);
    ~impl();

    void set_timeout_policy(const timeout_policy& policy) { timeout_policy_ = policy; }

    void set_timeout(const std::chrono::steady_clock::duration& duration) { timeout_ = duration; }

    http_client::request make_http_request(http::verb method,
                                           std::string_view path,
                                           const http::fields& headers);

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
                       const chunked_write_handler_type& chunked_write_handler,
                       bool retry) noexcept;

    net::awaitable<http_client::response_result>
    async_send_request_with_redirect(http_client::request& req,
                                     const body_setup_fn& body_setup,
                                     chunked_write_handler_type body_write);


    net::awaitable<http_client::response_result> async_download(http_client::request& req,
                                                                const fs::path& save_path);


private:
    net::awaitable<void> co_connect();
    net::awaitable<void> co_write_request(http::request<body::any_body>& req, bool headers_only);
    net::awaitable<http_client::response> co_read_response(const body_setup_fn& body_setup = {},
                                                           bool is_head                    = false);

    void expires_after(bool first = false);

    net::awaitable<http_client::response>
    async_send_request_impl(http_client::request& req,
                            const body_setup_fn& body_setup,
                            const chunked_write_handler_type& body_write);

public:
    net::any_io_executor executor_;
    tcp::resolver resolver_;
    timeout_policy timeout_policy_               = timeout_policy::overall;
    std::chrono::steady_clock::duration timeout_ = std::chrono::seconds(30);
    std::string host_;
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