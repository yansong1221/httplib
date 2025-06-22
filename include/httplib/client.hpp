#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <filesystem>
#include <limits>

namespace httplib {

class client
{
public:
    enum class timeout_policy
    {
        overall,
        step,
        never
    };

    using response = http::response<body::any_body>;
    using request  = http::request<body::any_body>;

    using response_result = boost::system::result<response>;

public:
    explicit client(net::io_context& ex, std::string_view host, uint16_t port);
    explicit client(const net::any_io_executor& ex, std::string_view host, uint16_t port);
    ~client();

    void set_timeout_policy(const timeout_policy& policy);

    void set_timeout(const std::chrono::steady_clock::duration& duration);
    void set_use_ssl(bool ssl);

    std::string_view host() const;
    uint16_t port() const;
    bool is_use_ssl() const;

public:
    net::awaitable<response_result> async_get(std::string_view path,
                                              const html::query_params& params = {},
                                              const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_head(std::string_view path,
                                               const http::fields& headers = http::fields());
    net::awaitable<response_result> async_post(std::string_view path,
                                               std::string_view body,
                                               const http::fields& headers = http::fields());
    net::awaitable<response_result> async_post(std::string_view path,
                                               boost::json::value&& body,
                                               const http::fields& headers = http::fields());
    response_result get(std::string_view path,
                        const html::query_params& params = {},
                        const http::fields& headers      = http::fields());

    void close();
    bool is_open() const;

private:
    class impl;
    impl* impl_;
};
} // namespace httplib