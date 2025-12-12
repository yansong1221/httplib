
#include "httplib/server/server.hpp"
#include "httplib/server/router.hpp"
#include "server_impl.h"

namespace httplib::server {

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

http_server::http_server(net::io_context& ioc)
    : http_server(ioc.get_executor())
{
}

http_server::http_server(const net::any_io_executor& ex)
    : impl_(std::make_unique<http_server_impl>(ex))
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

cobalt::task<boost::system::error_code> http_server::co_run()
{
    co_return co_await impl_->co_run();
}

void http_server::async_run()
{
    impl_->async_run();
}

void http_server::stop()
{
    impl_->stop();
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

const std::chrono::steady_clock::duration& http_server::read_timeout() const
{
    return impl_->read_timeout();
}

const std::chrono::steady_clock::duration& http_server::write_timeout() const
{
    return impl_->write_timeout();
}
std::shared_ptr<spdlog::logger> http_server::get_logger() const
{
    return impl_->get_logger();
}
void http_server::set_logger(std::shared_ptr<spdlog::logger> logger)
{
    impl_->set_logger(logger);
}

void http_server::use_ssl(const std::span<const char>& cert_file,
                          const std::span<const char>& key_file,
                          std::string passwd /*= {}*/)
{
    impl_->use_ssl(cert_file, key_file, passwd);
}

void http_server::use_ssl_file(const fs::path& cert_file,
                               const fs::path& key_file,
                               std::string passwd /*= {}*/)
{
    use_ssl(read_file_fast(cert_file), read_file_fast(key_file), passwd);
}

} // namespace httplib::server