#include "server_impl.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace httplib {

server_impl::server_impl(const net::any_io_executor& ex)
    : ex_(ex)
    , acceptor_(ex)
{
    auto console_sink                 = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::sinks_init_list sink_list = {console_sink};
    default_logger_ = std::make_shared<spdlog::logger>("httplib.server", sink_list);
    default_logger_->set_level(spdlog::level::info);
}

void server_impl::listen(std::string_view host,
                         uint16_t port,
                         int backlog /*= net::socket_base::max_listen_connections*/)
{
    tcp::resolver resolver(ex_);
    auto results = resolver.resolve(host, std::to_string(port));

    tcp::endpoint endp(*results.begin());
    acceptor_.open(endp.protocol());
    acceptor_.bind(endp);
    acceptor_.listen(backlog);

    auto listen_endp = local_endpoint();
    get_logger()->info(
        "Http Server Listen on: [{}:{}]", listen_endp.address().to_string(), listen_endp.port());
}

httplib::net::any_io_executor server_impl::get_executor() noexcept
{
    return ex_;
}

void server_impl::async_run()
{
    for (int i = 0; i < 32; ++i)
        net::co_spawn(ex_, co_run(), net::detached);
}

void server_impl::stop()
{
    boost::system::error_code ec;
    acceptor_.close(ec);
}

httplib::router& server_impl::router()
{
    return router_;
}

httplib::net::awaitable<boost::system::error_code> server_impl::co_run()
{
    boost::system::error_code ec;
    for (;;) {
        tcp::socket sock(ex_);
        co_await acceptor_.async_accept(sock, net_awaitable[ec]);
        if (ec) {
            get_logger()->trace("async_accept: {}", ec.message());
            break;
        }
        net::co_spawn(ex_, handle_accept(std::move(sock)), net::detached);
    }

    std::unique_lock<std::mutex> lck(session_mtx_);
    for (const auto& v : session_map_)
        v->abort();

    co_return ec;
}

httplib::net::awaitable<void> server_impl::handle_accept(tcp::socket sock)
{
    auto remote_endp = sock.remote_endpoint();
    auto local_endp  = sock.local_endpoint();
    get_logger()->trace(
        "accept new connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());

    auto session = std::make_shared<httplib::session>(std::move(sock), *this);
    {
        std::unique_lock<std::mutex> lck(session_mtx_);
        session_map_.insert(session);
    }
    try {
        co_await session->run();
    }
    catch (const std::exception& e) {
        get_logger()->error("session::run() exception: {}", e.what());
    }
    catch (...) {
        get_logger()->error("session::run() unknown exception");
    }
    {
        std::unique_lock<std::mutex> lck(session_mtx_);
        session_map_.erase(session);
    }
    get_logger()->trace(
        "close connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());
}

void server_impl::set_read_timeout(const std::chrono::steady_clock::duration& dur)
{
    read_timeout_ = dur;
}

void server_impl::set_write_timeout(const std::chrono::steady_clock::duration& dur)
{
    write_timeout_ = dur;
}

const std::chrono::steady_clock::duration& server_impl::read_timeout() const
{
    return read_timeout_;
}

const std::chrono::steady_clock::duration& server_impl::write_timeout() const
{
    return write_timeout_;
}

tcp::endpoint server_impl::local_endpoint() const
{
    boost::system::error_code ec;
    return acceptor_.local_endpoint(ec);
}

std::shared_ptr<spdlog::logger> server_impl::get_logger() const
{
    if (custom_logger_)
        return custom_logger_;
    return default_logger_;
}

void server_impl::set_logger(std::shared_ptr<spdlog::logger> logger)
{
    custom_logger_ = logger;
}

void server_impl::use_ssl(const net::const_buffer& cert_file,
                          const net::const_buffer& key_file,
                          std::string passwd /*= {}*/)
{
    SSLConfig conf;
    conf.cert_file.assign(static_cast<const uint8_t*>(cert_file.data()),
                          static_cast<const uint8_t*>(cert_file.data()) + cert_file.size());
    conf.key_file.assign(static_cast<const uint8_t*>(key_file.data()),
                         static_cast<const uint8_t*>(key_file.data()) + key_file.size());
    conf.passwd = passwd;

    ssl_conf_ = conf;
}

} // namespace httplib