#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/config.hpp"
#include "use_awaitable.hpp"
#include <boost/url.hpp>
#include <filesystem>
#include <limits>

namespace httplib
{

class client
{
public:
    enum class timeout_policy
    {
        overall,
        step,
        never
    };

public:
    explicit client(net::io_context& ex, std::string_view host, uint16_t port);
    explicit client(const net::any_io_executor& ex, std::string_view host, uint16_t port);
    ~client();

    void set_timeout_policy(const timeout_policy& policy);

    void set_timeout(const std::chrono::steady_clock::duration& duration);
    void set_use_ssl(bool ssl);

public:
    net::awaitable<http::response<body::any_body>> async_get(std::string_view path,
                                                             const http::fields& headers = http::fields());

    net::awaitable<http::response<body::any_body>> async_send_request(http::request<body::any_body>& req);

    net::awaitable<http::response<body::any_body>> async_head(std::string_view path,
                                                              const http::fields& headers = http::fields());


    net::awaitable<http::response<body::any_body>> async_post(std::string_view path,
                                                              std::string_view body,
                                                              const http::fields& headers = http::fields());

    http::response<body::any_body> get(std::string_view path, const http::fields& headers = http::fields());

    void close();
    bool is_connected();

private:
    class impl;
    std::unique_ptr<impl> impl_;
};

namespace detail
{
inline static uint16_t get_url_port(const boost::urls::url_view& url)
{
    uint16_t port = (url.scheme() == "https" ? 443 : 80);
    return url.has_port() ? url.port_number() : port;
}

inline static std::string get_url_path(const boost::urls::url_view& url)
{
    if (url.path().empty()) return "/";
    return url.path();
}
} // namespace detail

class http_downloader
{
public:
    net::awaitable<void> download(std::string_view url, std::filesystem::path save_path)
    {
        auto result = boost::urls::parse_uri(url);
        if (!result)
        {
            boost::throw_exception(boost::system::system_error(result.error()));
        }
        client cli(co_await net::this_coro::executor, result->host(), detail::get_url_port(*result));

        auto response = co_await cli.async_head(detail::get_url_path(*result));
        if (response.base().result_int() == 200 && response.has_content_length())
        {
            if (response[http::field::accept_ranges] == "bytes")
            {
            }
        }
    }
};
} // namespace httplib