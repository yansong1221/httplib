#include "httplib/client/proxy_client.hpp"
#include "proxy_client_impl.h"

namespace httplib::client
{

    proxy_client::proxy_client(net::io_context& ex, std::string_view host, uint16_t port, bool ssl)
        : proxy_client(ex.get_executor(), host, port, ssl)
    {
    }

    proxy_client::proxy_client(net::any_io_executor const& ex,
                               std::string_view host,
                               uint16_t port,
                               bool ssl)
        : impl_(std::make_shared<proxy_client::impl>(ex, host, port, ssl))
    {
    }

    proxy_client::~proxy_client() { abort(); }

    net::awaitable<boost::system::error_code>
    proxy_client::async_connect(std::string_view target, http::fields const& headers)
    {
        co_return co_await impl_->async_connect(target, headers);
    }

    net::awaitable<boost::system::result<std::size_t>>
    proxy_client::async_read_some(net::mutable_buffer const& buffer)
    {
        co_return co_await impl_->async_read_some(buffer);
    }

    net::awaitable<boost::system::error_code>
    proxy_client::async_write(net::const_buffer const& buffer)
    {
        co_return co_await impl_->async_write(buffer);
    }

    void
    proxy_client::close()
    {
        impl_->close();
    }

    void
    proxy_client::abort()
    {
        impl_->abort();
    }

    bool
    proxy_client::is_open() const noexcept
    {
        return impl_->is_open();
    }

    std::shared_ptr<spdlog::logger>
    proxy_client::logger() const
    {
        return impl_->logger();
    }

    void
    proxy_client::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        impl_->set_logger(std::move(logger));
    }

    void
    proxy_client::set_verify_ssl(bool verify)
    {
        impl_->set_verify_ssl(verify);
    }

    void
    proxy_client::set_ca_cert(std::string_view cert)
    {
        impl_->set_ca_cert(cert);
    }

} // namespace httplib::client
