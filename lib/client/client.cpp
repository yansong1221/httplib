#include "httplib/client/client.hpp"
#include "client_impl.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <utility>

namespace httplib::client {
namespace {

std::string make_target(std::string_view path, const html::query_params& params)
{
    std::string target(path);
    if (!params.empty()) {
        target += target.find('?') == std::string::npos ? "?" : "&";
        target += params.encoded();
    }
    return target;
}

template<typename AwaitableFactory>
http_client::response_result run_sync(const net::any_io_executor& ex, AwaitableFactory&& factory)
{
    auto future = net::co_spawn(ex, std::forward<AwaitableFactory>(factory)(), net::use_future);
    return future.get();
}

} // namespace

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
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::get, target, headers);
}


net::awaitable<http_client::response_result>
http_client::async_head(std::string_view path,
                        const html::query_params& params,
                        const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::head, target, headers);
}

net::awaitable<http_client::response_result> http_client::async_post(
    std::string_view path,
    const html::query_params& params /*= {}*/,
    const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::post, target, headers);
}

net::awaitable<http_client::response_result> http_client::async_post(
    std::string_view path,
    std::string_view body,
    const html::query_params& params /*= {}*/,
    const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::post, target, body, headers);
}

net::awaitable<http_client::response_result>
http_client::async_post(std::string_view path,
                        boost::json::value&& body,
                        const html::query_params& params /*= {}*/,
                        const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::post, target, std::move(body), headers);
}

net::awaitable<http_client::response_result>
http_client::async_put(std::string_view path,
                       const html::query_params& params,
                       const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::put, target, headers);
}

net::awaitable<http_client::response_result> http_client::async_put(
    std::string_view path,
    std::string_view body,
    const html::query_params& params /*= {}*/,
    const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::put, target, body, headers);
}

net::awaitable<http_client::response_result>
http_client::async_put(std::string_view path,
                       boost::json::value&& body,
                       const html::query_params& params /*= {}*/,
                       const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::put, target, std::move(body), headers);
}

net::awaitable<http_client::response_result> http_client::async_patch(
    std::string_view path,
    std::string_view body,
    const html::query_params& params /*= {}*/,
    const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::patch, target, body, headers);
}

net::awaitable<http_client::response_result>
http_client::async_patch(std::string_view path,
                         boost::json::value&& body,
                         const html::query_params& params /*= {}*/,
                         const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::patch, target, std::move(body), headers);
}

net::awaitable<http_client::response_result>
http_client::async_delete(std::string_view path,
                          const html::query_params& params,
                          const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::delete_, target, headers);
}

net::awaitable<http_client::response_result>
http_client::async_options(std::string_view path,
                           const html::query_params& params,
                           const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::options, target, headers);
}

net::awaitable<http_client::response_result>
http_client::async_connect(std::string_view path,
                            const html::query_params& params,
                            const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::connect, target, headers);
}

net::awaitable<http_client::response_result>
http_client::async_trace(std::string_view path,
                          const html::query_params& params,
                          const http::fields& headers /*= http::fields()*/)
{
    auto target = make_target(path, params);
    co_return co_await async_request(http::verb::trace, target, headers);
}

net::awaitable<http_client::response_result>
http_client::async_request(http::verb method,
                           std::string_view path,
                           const http::fields& headers /*= http::fields()*/)
{
    auto request = impl_->make_http_request(method, path, headers);
    co_return co_await impl_->async_send_request(request);
}

net::awaitable<http_client::response_result> http_client::async_request(
    http::verb method, std::string_view path, std::string_view body, const http::fields& headers)
{
    auto request    = impl_->make_http_request(method, path, headers);
    request.body()  = std::string(body);
    request.content_length(body.size());
    co_return co_await impl_->async_send_request(request);
}

net::awaitable<http_client::response_result>
http_client::async_request(http::verb method,
                           std::string_view path,
                           boost::json::value&& body,
                           const http::fields& headers)
{
    auto request = impl_->make_http_request(method, path, headers);
    request.set(http::field::content_type, "application/json");
    request.body() = std::move(body);
    request.prepare_payload();
    co_return co_await impl_->async_send_request(request);
}

http_client::response_result http_client::get(std::string_view path,
                                              const html::query_params& params,
                                              const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_get(path, params, headers); });
}

http_client::response_result http_client::head(std::string_view path,
                                               const html::query_params& params,
                                               const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_head(path, params, headers); });
}

http_client::response_result http_client::post(std::string_view path,
                                                const html::query_params& params /*= {}*/,
                                                const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_post(path, params, headers); });
}

http_client::response_result http_client::post(std::string_view path,
                                                std::string_view body,
                                                const html::query_params& params /*= {}*/,
                                                const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_post(path, body, params, headers); });
}

http_client::response_result http_client::post(std::string_view path,
                                               boost::json::value&& body,
                                               const html::query_params& params /*= {}*/,
                                               const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_,
                    [&] { return async_post(path, std::move(body), params, headers); });
}

http_client::response_result http_client::put(std::string_view path,
                                              const html::query_params& params,
                                              const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_put(path, params, headers); });
}

http_client::response_result http_client::put(std::string_view path,
                                              std::string_view body,
                                              const html::query_params& params /*= {}*/,
                                              const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_put(path, body, params, headers); });
}

http_client::response_result http_client::put(std::string_view path,
                                              boost::json::value&& body,
                                              const html::query_params& params /*= {}*/,
                                              const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_,
                    [&] { return async_put(path, std::move(body), params, headers); });
}

http_client::response_result http_client::patch(std::string_view path,
                                                std::string_view body,
                                                const html::query_params& params /*= {}*/,
                                                const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_patch(path, body, params, headers); });
}

http_client::response_result http_client::patch(std::string_view path,
                                                boost::json::value&& body,
                                                const html::query_params& params /*= {}*/,
                                                const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_,
                    [&] { return async_patch(path, std::move(body), params, headers); });
}

http_client::response_result http_client::del(std::string_view path,
                                              const html::query_params& params,
                                              const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_delete(path, params, headers); });
}

http_client::response_result http_client::options(std::string_view path,
                                                  const html::query_params& params,
                                                  const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_options(path, params, headers); });
}

http_client::response_result http_client::connect(std::string_view path,
                                                   const html::query_params& params,
                                                   const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_connect(path, params, headers); });
}

http_client::response_result http_client::trace(std::string_view path,
                                                 const html::query_params& params,
                                                 const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_trace(path, params, headers); });
}

http_client::response_result http_client::send_request(
    http::verb method, std::string_view path, const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_request(method, path, headers); });
}

http_client::response_result http_client::send_request(
    http::verb method,
    std::string_view path,
    std::string_view body,
    const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_, [&] { return async_request(method, path, body, headers); });
}

http_client::response_result http_client::send_request(
    http::verb method,
    std::string_view path,
    boost::json::value&& body,
    const http::fields& headers /*= http::fields()*/)
{
    return run_sync(impl_->executor_,
                    [&] { return async_request(method, path, std::move(body), headers); });
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
