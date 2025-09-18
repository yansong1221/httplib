
#include "httplib/server.hpp"
#include "httplib/router.hpp"
#include "server_impl.h"

namespace httplib {

server::server(net::io_context& ioc)
    : impl_(new server_impl(ioc.get_executor()))
{
}

server::server(const net::any_io_executor& ex)
    : impl_(new server_impl(ex))
{
}

server::~server()
{
    delete impl_;
}

net::any_io_executor server::get_executor() noexcept
{
    return impl_->get_executor();
}


server& server::listen(std::string_view host,
                       uint16_t port,
                       int backlog /*= net::socket_base::max_listen_connections*/)
{
    impl_->listen(host, port, backlog);
    return *this;
}

server& server::listen(uint16_t port, int backlog /*= net::socket_base::max_listen_connections*/)
{
    return listen("0.0.0.0", port, backlog);
}

net::awaitable<boost::system::error_code> server::co_run()
{
    co_return co_await impl_->co_run();
}

void server::async_run()
{
    impl_->async_run();
}

void server::stop()
{
    impl_->stop();
}
httplib::router& server::router()
{
    return impl_->router();
}

tcp::endpoint server::local_endpoint() const
{
    return impl_->local_endpoint();
}

void server::set_read_timeout(const std::chrono::steady_clock::duration& dur)
{
    impl_->set_read_timeout(dur);
}

void server::set_write_timeout(const std::chrono::steady_clock::duration& dur)
{
    impl_->set_write_timeout(dur);
}

const std::chrono::steady_clock::duration& server::read_timeout() const
{
    return impl_->read_timeout();
}

const std::chrono::steady_clock::duration& server::write_timeout() const
{
    return impl_->write_timeout();
}
std::shared_ptr<spdlog::logger> server::get_logger() const
{
    return impl_->get_logger();
}
void server::set_logger(std::shared_ptr<spdlog::logger> logger)
{
    impl_->set_logger(logger);
}

void server::use_ssl(const fs::path& cert_file,
                     const fs::path& key_file,
                     std::string passwd /*= {}*/)
{
    impl_->use_ssl(cert_file, key_file, passwd);
}

void server::set_websocket_open_handler(websocket_conn::open_handler_type&& handle)
{
    impl_->set_websocket_open_handler(std::move(handle));
}

void server::set_websocket_close_handler(websocket_conn::close_handler_type&& handle)
{
    impl_->set_websocket_close_handler(std::move(handle));
}

void server::set_websocket_message_handler(websocket_conn::message_handler_type&& handle)
{
    impl_->set_websocket_message_handler(std::move(handle));
}
} // namespace httplib