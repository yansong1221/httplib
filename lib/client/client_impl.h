#pragma once

#include "httplib/client/client.hpp"
#include "stream/http_stream.hpp"
#include <functional>
#include <spdlog/spdlog.h>

namespace httplib::client {

class http_client::impl
{
public:
    using body_setup_fn = std::function<void(body::any_body::value_type&)>;

    impl(const net::any_io_executor& ex, std::string_view host, uint16_t port, bool ssl);
    ~impl();

    void set_timeout_policy(const timeout_policy& policy) { timeout_policy_ = policy; }

    void set_timeout(const std::chrono::steady_clock::duration& duration) { timeout_ = duration; }

    http_client::request make_http_request(http::verb method,
                                           std::string_view path,
                                           const http::fields& headers);

    void set_chunk_handler(chunk_handler_type&& handler);

public:
    void close();
    bool is_open() const;

    std::shared_ptr<spdlog::logger> logger() const;
    void set_logger(std::shared_ptr<spdlog::logger> logger);

    net::awaitable<http_client::response_result> async_send_request(
        http_client::request& req,
        bool retry = true,
        const body_setup_fn& body_setup = {});
    void expires_after(bool first = false);

    net::awaitable<http_client::response> async_send_request_impl(
        http_client::request& req,
        const body_setup_fn& body_setup = {});

    net::awaitable<http_client::response_result> async_download(
        http_client::request& req,
        const fs::path& save_path);


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

    std::function<std::size_t(std::uint64_t, std::string_view, boost::system::error_code&)>
        chunk_handler_;

    std::shared_ptr<spdlog::logger> default_logger_;
    std::shared_ptr<spdlog::logger> custom_logger_;
};

} // namespace httplib::client