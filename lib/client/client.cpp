#include "httplib/client/client.hpp"
#include "client_impl.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <utility>

namespace httplib::client
{
    namespace
    {

        std::string
        make_target(std::string_view path, html::query_params const& params)
        {
            std::string target(path);
            if (!params.empty())
            {
                target += target.find('?') == std::string::npos ? "?" : "&";
                target += params.encoded();
            }
            return target;
        }

        template <typename Factory>
        http_client::response_result
        run_sync(net::any_io_executor const& ex, Factory&& factory)
        {
            auto future = net::co_spawn(ex, std::forward<Factory>(factory)(), net::use_future);
            return future.get();
        }

    } // namespace

    http_client::http_client(net::io_context& ex, std::string_view host, uint16_t port, bool ssl)
        : http_client(ex.get_executor(), host, port, ssl)
    {
    }

    http_client::http_client(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl)
        : impl_(std::make_shared<http_client::impl>(ex, host, port, ssl))
    {
    }

    http_client::~http_client() {}

    void
    http_client::set_timeout_policy(timeout_policy const& policy)
    {
        impl_->set_timeout_policy(policy);
    }

    void
    http_client::set_timeout(std::chrono::steady_clock::duration const& duration)
    {
        impl_->set_timeout(duration);
    }

    std::string_view
    http_client::host() const
    {
        return impl_->host_;
    }

    uint16_t
    http_client::port() const
    {
        return impl_->port_;
    }

    bool
    http_client::is_use_ssl() const
    {
        return impl_->use_ssl_;
    }

    std::shared_ptr<spdlog::logger>
    http_client::logger() const
    {
        return impl_->logger();
    }

    void
    http_client::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        impl_->set_logger(std::move(logger));
    }

    // =============================================================================
    // async_send overloads (core)
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http::verb method, std::string_view path, http::fields const& headers)
    {
        auto request = impl_->make_http_request(method, path, headers);
        co_return co_await impl_->async_send_request_with_redirect(request, nullptr, nullptr);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http::verb method,
                                    std::string_view path,
                                    std::string_view body,
                                    http::fields const& headers)
    {
        auto request = impl_->make_http_request(method, path, headers);
        request.body() = std::string(body);
        request.content_length(body.size());
        co_return co_await impl_->async_send_request_with_redirect(request, nullptr, nullptr);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http::verb method,
                                    std::string_view path,
                                    boost::json::value&& body,
                                    http::fields const& headers)
    {
        auto request = impl_->make_http_request(method, path, headers);
        request.set(http::field::content_type, "application/json");
        request.body() = std::move(body);
        request.prepare_payload();
        co_return co_await impl_->async_send_request_with_redirect(request, nullptr, nullptr);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http::verb method,
                                    std::string_view path,
                                    html::form_data&& body,
                                    http::fields const& headers)
    {
        auto request = impl_->make_http_request(method, path, headers);
        request.set(http::field::content_type, fmt::format("multipart/form-data; boundary={}", body.boundary));
        request.body() = std::move(body);
        request.prepare_payload();
        co_return co_await impl_->async_send_request_with_redirect(request, nullptr, nullptr);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http::verb method,
                                    std::string_view path,
                                    html::query_params&& body,
                                    http::fields const& headers)
    {
        auto request = impl_->make_http_request(method, path, headers);
        request.set(http::field::content_type, "application/x-www-form-urlencoded");
        request.body() = std::move(body);
        request.prepare_payload();
        co_return co_await impl_->async_send_request_with_redirect(request, nullptr, nullptr);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_file(http::verb method,
                                 std::string_view path,
                                 fs::path const& file_path,
                                 http::fields const& headers)
    {
        auto request = impl_->make_http_request(method, path, headers);
        body::file_body::value_type file_body;
        file_body.open(file_path, std::ios::in | std::ios::binary);
        request.body() = std::move(file_body);
        request.prepare_payload();
        co_return co_await impl_->async_send_request_with_redirect(request, nullptr, nullptr);
    }

    // =============================================================================
    // async_send_request with query params
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http::verb method,
                                    std::string_view path,
                                    html::query_params const& params,
                                    http::fields const& headers)
    {
        co_return co_await async_send_request(method, make_target(path, params), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http::verb method,
                                    std::string_view path,
                                    html::query_params const& params,
                                    std::string_view body,
                                    http::fields const& headers)
    {
        co_return co_await async_send_request(method, make_target(path, params), body, headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http::verb method,
                                    std::string_view path,
                                    html::query_params const& params,
                                    boost::json::value&& body,
                                    http::fields const& headers)
    {
        co_return co_await async_send_request(method, make_target(path, params), std::move(body), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http::verb method,
                                    std::string_view path,
                                    html::query_params const& params,
                                    html::form_data&& body,
                                    http::fields const& headers)
    {
        co_return co_await async_send_request(method, make_target(path, params), std::move(body), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http::verb method,
                                    std::string_view path,
                                    html::query_params const& params,
                                    html::query_params&& body,
                                    http::fields const& headers)
    {
        co_return co_await async_send_request(method, make_target(path, params), std::move(body), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_file(http::verb method,
                                 std::string_view path,
                                 html::query_params const& params,
                                 fs::path const& file_path,
                                 http::fields const& headers)
    {
        co_return co_await async_send_file(method, make_target(path, params), file_path, headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_chunked_request(http::verb method,
                                            std::string_view path,
                                            chunked_write_handler_type handler,
                                            html::query_params const& params,
                                            http::fields const& headers)
    {
        auto req = impl_->make_http_request(method, path, headers);
        req.set(http::field::transfer_encoding, "chunked");
        req.chunked(true);
        co_return co_await impl_->async_send_request_with_redirect(req, nullptr, std::move(handler));
    }

    write_session&
    http_client::writer()
    {
        return impl_->write_session();
    }

    std::shared_ptr<read_session>
    http_client::reader()
    {
        return impl_->read_session();
    }

    http_client::response_result
    http_client::send_chunked_request(http::verb method,
                                      std::string_view path,
                                      chunked_write_handler_type handler,
                                      html::query_params const& params,
                                      http::fields const& headers)
    {
        auto future = net::co_spawn(impl_->executor_,
                                    async_send_chunked_request(method, path, std::move(handler), params, headers),
                                    net::use_future);
        return future.get();
    }

    // =============================================================================
    // HTTP method shorthands (no body)
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_get(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::get, make_target(path, params), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_head(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::head, make_target(path, params), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_post(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::post, make_target(path, params), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_put(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::put, make_target(path, params), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_patch(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::patch, make_target(path, params), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_del(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::delete_, make_target(path, params), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_options(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::options, make_target(path, params), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_connect(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::connect, make_target(path, params), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_trace(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::trace, make_target(path, params), headers);
    }

    // =============================================================================
    // HTTP method shorthands (with body)
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_post(std::string_view path,
                            std::string_view body,
                            html::query_params const& params,
                            http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::post, make_target(path, params), body, headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_post(std::string_view path,
                            boost::json::value&& body,
                            html::query_params const& params,
                            http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::post, make_target(path, params), std::move(body), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_put(std::string_view path,
                           std::string_view body,
                           html::query_params const& params,
                           http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::put, make_target(path, params), body, headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_put(std::string_view path,
                           boost::json::value&& body,
                           html::query_params const& params,
                           http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::put, make_target(path, params), std::move(body), headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_patch(std::string_view path,
                             std::string_view body,
                             html::query_params const& params,
                             http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::patch, make_target(path, params), body, headers);
    }

    net::awaitable<http_client::response_result>
    http_client::async_patch(std::string_view path,
                             boost::json::value&& body,
                             html::query_params const& params,
                             http::fields const& headers)
    {
        co_return co_await async_send_request(http::verb::patch, make_target(path, params), std::move(body), headers);
    }

    // =============================================================================
    // Download
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_download(http::verb method,
                                std::string_view path,
                                fs::path const& save_path,
                                http::fields const& headers)
    {
        auto request = impl_->make_http_request(method, path, headers);
        co_return co_await impl_->async_download(request, save_path);
    }

    // =============================================================================
    // send overloads (sync)
    // =============================================================================

    http_client::response_result
    http_client::send_request(http::verb method, std::string_view path, http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_send_request(method, path, headers); });
    }

    http_client::response_result
    http_client::send_request(http::verb method,
                              std::string_view path,
                              std::string_view body,
                              http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_send_request(method, path, body, headers); });
    }

    http_client::response_result
    http_client::send_request(http::verb method,
                              std::string_view path,
                              boost::json::value&& body,
                              http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_send_request(method, path, std::move(body), headers); });
    }

    http_client::response_result
    http_client::send_request(http::verb method,
                              std::string_view path,
                              html::form_data&& body,
                              http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_send_request(method, path, std::move(body), headers); });
    }

    http_client::response_result
    http_client::send_request(http::verb method,
                              std::string_view path,
                              html::query_params&& body,
                              http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_send_request(method, path, std::move(body), headers); });
    }

    http_client::response_result
    http_client::send_file(http::verb method,
                           std::string_view path,
                           fs::path const& file_path,
                           http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_send_file(method, path, file_path, headers); });
    }

    // =============================================================================
    // send_request with query params (sync)
    // =============================================================================

    http_client::response_result
    http_client::send_request(http::verb method,
                              std::string_view path,
                              html::query_params const& params,
                              http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_send_request(method, path, params, headers); });
    }

    http_client::response_result
    http_client::send_request(http::verb method,
                              std::string_view path,
                              html::query_params const& params,
                              std::string_view body,
                              http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_send_request(method, path, params, body, headers); });
    }

    http_client::response_result
    http_client::send_request(http::verb method,
                              std::string_view path,
                              html::query_params const& params,
                              boost::json::value&& body,
                              http::fields const& headers)
    {
        return run_sync(impl_->executor_,
                        [&] { return async_send_request(method, path, params, std::move(body), headers); });
    }

    http_client::response_result
    http_client::send_request(http::verb method,
                              std::string_view path,
                              html::query_params const& params,
                              html::form_data&& body,
                              http::fields const& headers)
    {
        return run_sync(impl_->executor_,
                        [&] { return async_send_request(method, path, params, std::move(body), headers); });
    }

    http_client::response_result
    http_client::send_request(http::verb method,
                              std::string_view path,
                              html::query_params const& params,
                              html::query_params&& body,
                              http::fields const& headers)
    {
        return run_sync(impl_->executor_,
                        [&] { return async_send_request(method, path, params, std::move(body), headers); });
    }

    http_client::response_result
    http_client::send_file(http::verb method,
                           std::string_view path,
                           html::query_params const& params,
                           fs::path const& file_path,
                           http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_send_file(method, path, params, file_path, headers); });
    }

    // =============================================================================
    // HTTP method shorthands (sync, no body)
    // =============================================================================

    http_client::response_result
    http_client::get(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_get(path, params, headers); });
    }

    http_client::response_result
    http_client::head(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_head(path, params, headers); });
    }

    http_client::response_result
    http_client::post(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_post(path, params, headers); });
    }

    http_client::response_result
    http_client::put(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_put(path, params, headers); });
    }

    http_client::response_result
    http_client::patch(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_patch(path, params, headers); });
    }

    http_client::response_result
    http_client::del(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_del(path, params, headers); });
    }

    http_client::response_result
    http_client::options(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_options(path, params, headers); });
    }

    http_client::response_result
    http_client::connect(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_connect(path, params, headers); });
    }

    http_client::response_result
    http_client::trace(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_trace(path, params, headers); });
    }

    // =============================================================================
    // HTTP method shorthands (sync, with body)
    // =============================================================================

    http_client::response_result
    http_client::post(std::string_view path,
                      std::string_view body,
                      html::query_params const& params,
                      http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_post(path, body, params, headers); });
    }

    http_client::response_result
    http_client::post(std::string_view path,
                      boost::json::value&& body,
                      html::query_params const& params,
                      http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_post(path, std::move(body), params, headers); });
    }

    http_client::response_result
    http_client::put(std::string_view path,
                     std::string_view body,
                     html::query_params const& params,
                     http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_put(path, body, params, headers); });
    }

    http_client::response_result
    http_client::put(std::string_view path,
                     boost::json::value&& body,
                     html::query_params const& params,
                     http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_put(path, std::move(body), params, headers); });
    }

    http_client::response_result
    http_client::patch(std::string_view path,
                       std::string_view body,
                       html::query_params const& params,
                       http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_patch(path, body, params, headers); });
    }

    http_client::response_result
    http_client::patch(std::string_view path,
                       boost::json::value&& body,
                       html::query_params const& params,
                       http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_patch(path, std::move(body), params, headers); });
    }

    // =============================================================================
    // Download (sync)
    // =============================================================================

    http_client::response_result
    http_client::download(http::verb method,
                          std::string_view path,
                          fs::path const& save_path,
                          http::fields const& headers)
    {
        return run_sync(impl_->executor_, [&] { return async_download(method, path, save_path, headers); });
    }

    void
    http_client::close()
    {
        impl_->close();
    }

    bool
    http_client::is_open() const
    {
        return impl_->is_open();
    }

    void
    http_client::set_chunked_read_handler(chunked_read_handler_type&& handler)
    {
        impl_->set_chunked_read_handler(std::move(handler));
    }

    void
    http_client::set_sse_read_handler(sse_read_handler_type&& handler)
    {
        impl_->set_sse_read_handler(std::move(handler));
    }

    void
    http_client::set_ndjson_read_handler(ndjson_read_handler_type&& handler)
    {
        impl_->set_ndjson_read_handler(std::move(handler));
    }

    void
    http_client::set_max_redirects(int n)
    {
        impl_->set_max_redirects(n);
    }

} // namespace httplib::client
