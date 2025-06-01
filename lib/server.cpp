
#include "httplib/server.hpp"
#include "httplib/router.hpp"
#include "httplib/setting.hpp"
#include "server_impl.h"

namespace httplib {

server::server(uint32_t num_threads)
    : impl_(new impl(*this, num_threads))
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


server::setting& server::option()
{
    return impl_->option();
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

void server::run()
{
    impl_->async_run();
    impl_->wait();
}

void server::async_run()
{
    impl_->async_run();
}

void server::wait()
{
    impl_->wait();
}

void server::stop()
{
    impl_->stop();
}
httplib::router& server::router()
{
    return impl_->router();
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

} // namespace httplib