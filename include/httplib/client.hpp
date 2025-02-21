#pragma once
#include "httplib/config.hpp"
#include "httplib/stream/http_stream.hpp"
#include "use_awaitable.hpp"
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast.hpp>
#include <boost/url.hpp>
#include <filesystem>
#include <limits>

namespace httplib {

class client {
public:
    enum class timeout_policy {
        overall,
        step,
        never
    };

public:
    client(net::io_context &ex, std::string_view host, uint16_t port)
        : client(ex.get_executor(), host, port) {}

    client(const net::any_io_executor &ex, std::string_view host, uint16_t port)
        : executor_(ex), resolver_(ex), host_(host), port_(port) {}

    void set_timeout_policy(const timeout_policy &policy) {
        timeout_policy_ = policy;
    }

    void set_timeout(const std::chrono::steady_clock::duration &duration) {
        timeout_ = duration;
    }
    void set_use_ssl(bool ssl) {
        use_ssl_ = ssl;
    }

    template<typename ResponseBody = http::string_body>
    auto async_get(std::string_view path, const http::fields headers = http::fields()) {
        auto request = make_http_request<http::empty_body>(http::verb::get, path, headers);
        return async_send_request<ResponseBody>(std::move(request));
    }

    auto async_head(std::string_view path, const http::fields headers = http::fields()) {
        auto request = make_http_request<http::empty_body>(http::verb::head, path, headers);
        return async_send_request<http::empty_body>(std::move(request));
    }

    template<typename ResponseBody = http::string_body>
    auto async_post(std::string_view path, std::string_view body,
                    const http::fields headers = http::fields()) {
        auto request =
            make_http_request<http::span_body<const char>>(http::verb::post, path, headers);
        request.body() = body;
        request.prepare_payload();
        return async_send_request<ResponseBody>(std::move(request));
    }

    template<typename ResponseBody = http::string_body>
    auto get(std::string_view path, const http::fields headers = http::fields()) {
        auto future =
            net::co_spawn(executor_, async_get<ResponseBody>(path, headers), net::use_future);
        return future.get();
    }

    void close() {
        resolver_.cancel();
        if (variant_stream_) {
            variant_stream_->expires_never();
            boost::system::error_code ec;
            variant_stream_->close(ec);
        }
    }

    bool is_connected() {
        return variant_stream_ && variant_stream_->is_connected();
    }

private:
    template<typename RequestBody>
    auto make_http_request(http::verb method, std::string_view path, const http::fields &headers) {
        http::request<RequestBody> req(method, path, 11);
        req.set(http::field::host, host_);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        for (const auto &field : headers)
            req.set(field.name_string(), field.value());
        return std::move(req);
    }

    template<typename ResponseBody, typename RequestBody>
    net::awaitable<http::response<ResponseBody>>
    async_send_request(http::request<RequestBody> request) {
        try {
            bool first_set_timer = true;
            auto reset_step_timer = [&]() {
                if (timeout_policy_ == timeout_policy::step)
                    variant_stream_->expires_after(timeout_);
                else if (timeout_policy_ == timeout_policy::never)
                    variant_stream_->expires_never();
                else if (timeout_policy_ == timeout_policy::overall) {
                    if (!first_set_timer)
                        return;
                    variant_stream_->expires_after(timeout_);
                }
                first_set_timer = false;
            };
            
            // Set up an HTTP GET request message
            if (!is_connected()) {
                if (variant_stream_) {
                    boost::system::error_code ec;
                    variant_stream_->close(ec);
                }

                auto endpoints = co_await resolver_.async_resolve(host_, std::to_string(port_),
                                                                  net::use_awaitable);
                reset_step_timer();
                if (use_ssl_) {
#ifdef HTTLIP_ENABLED_SSL
                    unsigned long ssl_options = ssl::context::default_workarounds |
                                                ssl::context::no_sslv2 |
                                                ssl::context::single_dh_use;

                    auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
                    ssl_ctx->set_options(ssl_options);

                    ssl_http_stream stream(co_await net::this_coro::executor, ssl_ctx);
                    co_await beast::get_lowest_layer(stream).async_connect(endpoints,
                                                                           net::use_awaitable);
                    co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

                    variant_stream_ = std::make_unique<http_variant_stream_type>(
                        ssl_http_stream(std::move(stream)));
#endif
                } else {
                    http_stream stream(co_await net::this_coro::executor);
                    co_await stream.async_connect(endpoints, net::use_awaitable);
                    variant_stream_ =
                        std::make_unique<http_variant_stream_type>(http_stream(std::move(stream)));
                }
            }
            http::request_serializer<RequestBody> serializer(request);
            while (!serializer.is_done()) {
                reset_step_timer();
                co_await http::async_write_some(*variant_stream_, serializer);
            }

            http::response_parser<ResponseBody> parser;
            beast::flat_buffer buffer;
            while ((request.method() == http::verb::head && !parser.is_header_done()) ||
                   (request.method() != http::verb::head && !parser.is_done())) {
                reset_step_timer();
                co_await http::async_read_some(*variant_stream_, buffer, parser);
            }

            variant_stream_->expires_never();
            co_return parser.release();
        } catch (...) {
            close();
            std::rethrow_exception(std::current_exception());
        }
    }

private:
    net::any_io_executor executor_;
    tcp::resolver resolver_;
    timeout_policy timeout_policy_ = timeout_policy::overall;
    std::chrono::steady_clock::duration timeout_ = std::chrono::seconds(30);
    std::string host_;
    uint16_t port_ = 0;
    std::unique_ptr<http_variant_stream_type> variant_stream_;
    bool use_ssl_ = false;
};

namespace detail {
inline static uint16_t get_url_port(const boost::urls::url_view &url) {
    uint16_t port = (url.scheme() == "https" ? 443 : 80);
    return url.has_port() ? url.port_number() : port;
}

inline static std::string get_url_path(const boost::urls::url_view &url) {
    if (url.path().empty())
        return "/";
    return url.path();
}
} // namespace detail

class http_downloader {
public:
    net::awaitable<void> download(std::string_view url, std::filesystem::path save_path) {
        auto result = boost::urls::parse_uri(url);
        if (!result) {
            boost::throw_exception(boost::system::system_error(result.error()));
        }
        client cli(co_await net::this_coro::executor, result->host(),
                   detail::get_url_port(*result));

        auto response = co_await cli.async_head(detail::get_url_path(*result));
        if (response.base().result_int() == 200 && response.has_content_length()) {
            if (response[http::field::accept_ranges] == "bytes") {
            }
        }
    }
};
} // namespace httplib