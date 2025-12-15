#include "httplib/client/client.hpp"
#include "client_impl.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

namespace httplib::client {

http_client::http_client(net::io_context& ex, std::string_view host, uint16_t port, bool ssl)
    : http_client(ex.get_executor(), host, port, ssl)
{
}

http_client::http_client(const net::any_io_executor& ex,
                         std::string_view host,
                         uint16_t port,
                         bool ssl)
    : impl_(std::make_unique<http_client::impl>(ex, host, port, ssl))
{
}

http_client::~http_client()
{
}

void http_client::set_timeout_policy(const timeout_policy& policy)
{
    impl_->set_timeout_policy(policy);
}

void http_client::set_timeout(const std::chrono::steady_clock::duration& duration)
{
    impl_->set_timeout(duration);
}

std::string_view http_client::host() const
{
    return impl_->host_;
}

uint16_t http_client::port() const
{
    return impl_->port_;
}

bool http_client::is_use_ssl() const
{
    return impl_->use_ssl_;
}

net::awaitable<http_client::response_result>
http_client::async_get(std::string_view path,
                       const html::query_params& params,
                       const http::fields& headers /*= http::fields()*/)
{
    std::string target(path);
    if (!params.empty()) {
        target += "?";
        target += params.encoded();
    }
    auto req = impl_->make_http_request(http::verb::get, target, headers);
    co_return co_await impl_->async_send_request(req);
}


net::awaitable<http_client::response_result>
http_client::async_head(std::string_view path, const http::fields& headers /*= http::fields()*/)
{
    auto req = impl_->make_http_request(http::verb::head, path, headers);
    co_return co_await impl_->async_send_request(req);
}

net::awaitable<http_client::response_result> http_client::async_post(
    std::string_view path, std::string_view body, const http::fields& headers /*= http::fields()*/)
{
    auto request = impl_->make_http_request(http::verb::post, path, headers);
    request.content_length(body.size());
    request.body() = std::string(body);
    co_return co_await impl_->async_send_request(request);
}

net::awaitable<http_client::response_result>
http_client::async_post(std::string_view path,
                        boost::json::value&& body,
                        const http::fields& headers /*= http::fields()*/)
{
    auto request = impl_->make_http_request(http::verb::post, path, headers);
    request.set(http::field::content_type, "application/json");
    request.body() = std::move(body);
    request.prepare_payload();
    co_return co_await impl_->async_send_request(request);
}

http_client::response_result http_client::get(std::string_view path,
                                              const html::query_params& params,
                                              const http::fields& headers /*= http::fields()*/)
{
    auto future =
        net::co_spawn(impl_->executor_, async_get(path, params, headers), net::use_future);
    return future.get();
}

void http_client::close()
{
    impl_->close();
}

bool http_client::is_open() const
{
    return impl_->is_open();
}

void http_client::set_chunk_handler(chunk_handler_type&& handler)
{
    impl_->set_chunk_handler(std::move(handler));
}

} // namespace httplib::client