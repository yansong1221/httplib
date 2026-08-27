#include "httplib/client/client.hpp"
#include "client_impl.h"
#include <boost/url.hpp>
#include <stdexcept>
#include <utility>

namespace httplib::client
{
    http_client::http_client(net::io_context& ex, std::string_view host, uint16_t port, bool ssl)
        : http_client(ex.get_executor(), host, port, ssl)
    {
    }

    http_client::http_client(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl)
        : impl_(std::make_shared<http_client::impl>(ex, host, port, ssl))
    {
    }

    http_client::http_client(net::io_context& ex, std::string_view url) : http_client(ex.get_executor(), url) {}

    http_client::http_client(net::any_io_executor const& ex, std::string_view url) : impl_(nullptr)
    {
        auto r = boost::urls::parse_uri(url);
        if (!r)
        {
            throw std::invalid_argument(std::format("invalid url: {}", url));
        }

        auto const& u = *r;

        auto port = (u.scheme_id() == boost::urls::scheme::https ? 443 : 80);
        port = u.has_port() ? u.port_number() : port;

        impl_ = std::make_shared<http_client::impl>(ex, u.host(), port, u.scheme_id() == boost::urls::scheme::https);
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
    // core send
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_send_request(http_client::request req)
    {
        auto result = co_await impl_->async_send_request_lazy_with_redirect(req);
        if (result.has_error())
        {
            co_return result.error();
        }
        co_return co_await result->read_body();
    }

    net::awaitable<http_client::response_result>
    http_client::async_send_request_lazy(http_client::request req)
    {
        co_return co_await impl_->async_send_request_lazy_with_redirect(req);
    }

    // =============================================================================
    // lazy request
    // =============================================================================

    std::shared_ptr<lazy_request>
    http_client::create_lazy_request()
    {
        return impl_->create_lazy_request();
    }

    // =============================================================================
    // HTTP method shorthands (no body)
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_get(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(request(http::verb::get, path, params, headers));
    }

    net::awaitable<http_client::response_result>
    http_client::async_head(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(request(http::verb::head, path, params, headers));
    }

    net::awaitable<http_client::response_result>
    http_client::async_post(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(request(http::verb::post, path, params, headers));
    }

    net::awaitable<http_client::response_result>
    http_client::async_put(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(request(http::verb::put, path, params, headers));
    }

    net::awaitable<http_client::response_result>
    http_client::async_patch(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(request(http::verb::patch, path, params, headers));
    }

    net::awaitable<http_client::response_result>
    http_client::async_del(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(request(http::verb::delete_, path, params, headers));
    }

    net::awaitable<http_client::response_result>
    http_client::async_options(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        co_return co_await async_send_request(request(http::verb::options, path, params, headers));
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
        auto req = request(http::verb::post, path, params, headers);
        req.set_body(body);
        co_return co_await async_send_request(std::move(req));
    }

    net::awaitable<http_client::response_result>
    http_client::async_post(std::string_view path,
                            boost::json::value&& body,
                            html::query_params const& params,
                            http::fields const& headers)
    {
        auto req = request(http::verb::post, path, params, headers);
        req.set_body(std::move(body));
        co_return co_await async_send_request(std::move(req));
    }

    net::awaitable<http_client::response_result>
    http_client::async_put(std::string_view path,
                           std::string_view body,
                           html::query_params const& params,
                           http::fields const& headers)
    {
        auto req = request(http::verb::put, path, params, headers);
        req.set_body(body);
        co_return co_await async_send_request(std::move(req));
    }

    net::awaitable<http_client::response_result>
    http_client::async_put(std::string_view path,
                           boost::json::value&& body,
                           html::query_params const& params,
                           http::fields const& headers)
    {
        auto req = request(http::verb::put, path, params, headers);
        req.set_body(std::move(body));
        co_return co_await async_send_request(std::move(req));
    }

    net::awaitable<http_client::response_result>
    http_client::async_patch(std::string_view path,
                             std::string_view body,
                             html::query_params const& params,
                             http::fields const& headers)
    {
        auto req = request(http::verb::patch, path, params, headers);
        req.set_body(body);
        co_return co_await async_send_request(std::move(req));
    }

    net::awaitable<http_client::response_result>
    http_client::async_patch(std::string_view path,
                             boost::json::value&& body,
                             html::query_params const& params,
                             http::fields const& headers)
    {
        auto req = request(http::verb::patch, path, params, headers);
        req.set_body(std::move(body));
        co_return co_await async_send_request(std::move(req));
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
        auto req = http_client::request(method, path, headers);
        auto result = co_await impl_->async_send_request_lazy_with_redirect(req);
        if (result.has_error())
        {
            co_return result.error();
        }

        if (auto ec = co_await result->read_to_file(save_path); ec)
        {
            co_return ec;
        }
        co_return result;
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

    bool
    http_client::has_active_session() const
    {
        return impl_->has_active_session();
    }

    bool
    http_client::is_alive() const
    {
        return impl_->is_alive();
    }

    void
    http_client::set_max_redirects(int n)
    {
        impl_->set_max_redirects(n);
    }

    void
    http_client::set_verify_ssl(bool verify)
    {
        impl_->verify_ssl_ = verify;
    }

    void
    http_client::set_ca_cert(std::string_view cert)
    {
        impl_->ca_cert_ = cert;
    }

    net::any_io_executor
    http_client::get_executor() const
    {
        return impl_->executor_;
    }

} // namespace httplib::client
