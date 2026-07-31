
#include "httplib/server/server.hpp"
#include "httplib/server/router.hpp"
#include "server_impl.h"

namespace httplib::server {
namespace detail {
static std::string read_file_fast(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open file: " + path.string());

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::string buffer(size, 0);
    if (!file.read(buffer.data(), size))
        throw std::runtime_error("Failed to read file: " + path.string());

    return buffer;
}
} // namespace detail


http_server::http_server(net::io_context& ioc)
    : http_server(ioc.get_executor())
{
}

http_server::http_server(const net::any_io_executor& ex)
    : impl_(std::make_shared<impl>(ex))
{
}

http_server::~http_server()
{
}

net::any_io_executor http_server::get_executor() noexcept
{
    return impl_->get_executor();
}


http_server& http_server::listen(std::string_view host,
                                 uint16_t port,
                                 int backlog /*= net::socket_base::max_listen_connections*/)
{
    impl_->listen(host, port, backlog);
    return *this;
}

http_server& http_server::listen(uint16_t port,
                                 int backlog /*= net::socket_base::max_listen_connections*/)
{
    return listen("0.0.0.0", port, backlog);
}

net::awaitable<boost::system::error_code> http_server::async_run()
{
    co_return co_await impl_->async_run();
}

std::shared_future<boost::system::error_code> http_server::run()
{
    return impl_->run();
}

std::shared_future<void> http_server::stop()
{
    return impl_->stop();
}
router& http_server::router()
{
    return impl_->router();
}

tcp::endpoint http_server::local_endpoint() const
{
    return impl_->local_endpoint();
}

void http_server::set_read_timeout(const std::chrono::steady_clock::duration& dur)
{
    impl_->set_read_timeout(dur);
}

void http_server::set_write_timeout(const std::chrono::steady_clock::duration& dur)
{
    impl_->set_write_timeout(dur);
}

std::chrono::steady_clock::duration http_server::read_timeout() const
{
    return impl_->read_timeout();
}

std::chrono::steady_clock::duration http_server::write_timeout() const
{
    return impl_->write_timeout();
}
std::shared_ptr<spdlog::logger> http_server::logger() const
{
    return impl_->logger();
}
void http_server::set_logger(std::shared_ptr<spdlog::logger> logger)
{
    impl_->set_logger(logger);
}

void http_server::set_compress_content_types(std::function<bool(std::string_view)> predicate)
{
    impl_->set_compress_content_types(std::move(predicate));
}

void http_server::set_upload_dir(const fs::path& dir)
{
    impl_->set_upload_dir(dir);
}

void http_server::set_upload_file_limit(std::uint64_t max_bytes)
{
    impl_->set_upload_file_limit(max_bytes);
}

void http_server::set_reverse_proxy(std::string_view key,
                                     std::string_view upstream_host,
                                     uint16_t upstream_port,
                                     bool upstream_ssl)
{
    impl_->set_reverse_proxy(key, upstream_host, upstream_port, upstream_ssl);
}

void http_server::set_ssl(const std::span<const char>& cert_file,
                          const std::span<const char>& key_file,
                          std::string passwd /*= {}*/)
{
    impl_->use_ssl(cert_file, key_file, passwd);
}

void http_server::set_ssl_file(const fs::path& cert_file,
                               const fs::path& key_file,
                               std::string passwd /*= {}*/)
{
    set_ssl(detail::read_file_fast(cert_file), detail::read_file_fast(key_file), passwd);
}

httplib::server::http_server::impl* http_server::get_impl()
{
    return impl_.get();
}

const httplib::server::http_server::impl* http_server::get_impl() const
{
    return impl_.get();
}

net::awaitable<void> http_server::async_stop()
{
    co_return co_await impl_->async_stop();
}

} // namespace httplib::server