#include "server_impl.h"

namespace httplib {

server::impl::impl(server& self, uint32_t num_threads)
    : self_(self)
    , pool_(num_threads)
    , acceptor_(pool_)
    , router_(option_)
{
}

void server::impl::listen(std::string_view host,
                          uint16_t port,
                          int backlog /*= net::socket_base::max_listen_connections*/)
{
    tcp::resolver resolver(pool_);
    auto results = resolver.resolve(host, std::to_string(port));

    tcp::endpoint endp(*results.begin());
    acceptor_.open(endp.protocol());
    acceptor_.bind(endp);
    acceptor_.listen(backlog);
    option_.get_logger()->info(
        "Server Listen on: [{}:{}]", endp.address().to_string(), endp.port());
}

httplib::net::any_io_executor server::impl::get_executor() noexcept
{
    return pool_.get_executor();
}

httplib::server::setting& server::impl::option()
{
    return option_;
}

void server::impl::async_run()
{
    net::co_spawn(pool_, accept_loop(), net::detached);
}

void server::impl::wait()
{
    pool_.wait();
    session_map_.clear();
}

void server::impl::stop()
{
    boost::system::error_code ec;
    acceptor_.close(ec);
    {
        std::unique_lock<std::mutex> lck(session_mtx_);
        for (const auto& v : session_map_)
            v->abort();
    }
    pool_.stop();
}

httplib::router& server::impl::router()
{
    return router_;
}

httplib::net::awaitable<void> server::impl::accept_loop()
{
    boost::system::error_code ec;
    for (;;) {
        tcp::socket sock(pool_);
        co_await acceptor_.async_accept(sock, net_awaitable[ec]);
        if (ec) {
            option_.get_logger()->trace("async_accept: {}", ec.message());
            co_return;
        }
        net::co_spawn(pool_, handle_accept(std::move(sock)), net::detached);
    }
}

httplib::net::awaitable<void> server::impl::handle_accept(tcp::socket&& sock)
{
    auto remote_endp = sock.remote_endpoint();
    auto local_endp  = sock.local_endpoint();
    option_.get_logger()->trace(
        "accept new connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());

    auto session = std::make_shared<httplib::session>(std::move(sock), self_);
    {
        std::unique_lock<std::mutex> lck(session_mtx_);
        session_map_.insert(session);
    }
    try {
        co_await session->run();
    }
    catch (const std::exception& e) {
        option_.get_logger()->error("session::run() exception: {}", e.what());
    }
    catch (...) {
        option_.get_logger()->error("session::run() unknown exception");
    }
    {
        std::unique_lock<std::mutex> lck(session_mtx_);
        session_map_.erase(session);
    }
    option_.get_logger()->trace(
        "close connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());
}

void server::impl::set_read_timeout(const std::chrono::steady_clock::duration& dur)
{
    read_timeout_ = dur;
}

void server::impl::set_write_timeout(const std::chrono::steady_clock::duration& dur)
{
    write_timeout_ = dur;
}

const std::chrono::steady_clock::duration& server::impl::read_timeout() const
{
    return read_timeout_;
}

const std::chrono::steady_clock::duration& server::impl::write_timeout() const
{
    return write_timeout_;
}

} // namespace httplib