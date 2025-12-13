#include "server_impl.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>


#include <boost/asio.hpp>
#include <boost/cobalt/io/sleep.hpp>

namespace httplib::server {

http_server_impl::http_server_impl(const net::any_io_executor& ex)
    : ex_(ex)
    , acceptor_(ex)
{
    auto console_sink                 = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::sinks_init_list sink_list = {console_sink};
    default_logger_ = std::make_shared<spdlog::logger>("httplib.server", sink_list);
    default_logger_->set_level(spdlog::level::info);
}

void http_server_impl::listen(std::string_view host,
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

net::any_io_executor http_server_impl::get_executor() noexcept
{
    return ex_;
}

void http_server_impl::async_run()
{
    cobalt::spawn(net::make_strand(ex_), co_run(), net::detached);
}

void http_server_impl::stop()
{
    boost::system::error_code ec;
    acceptor_.close(ec);
}

router_impl& http_server_impl::router()
{
    return router_;
}

cobalt::task<boost::system::error_code> http_server_impl::co_run()
{
    cobalt::wait_group task_group;

    boost::system::error_code ec;
    for (int i = 0; i < 32; i++) {
        task_group.push_back(accept_socket(boost::asio::executor_arg, ex_));
    }
    co_await task_group.wait();

    {
        std::lock_guard<std::mutex> lck(session_mutex_);
        for (const auto& conn : session_map_)
            conn->abort();
    }
    co_return ec;
}
cobalt::promise<void> http_server_impl::accept_socket(boost::asio::executor_arg_t, cobalt::executor)
{
    boost::system::error_code ec;
    auto executor = co_await cobalt::this_coro::executor;
    for (;;) {
        tcp::socket sock(executor);
        co_await acceptor_.async_accept(sock, boost::asio::redirect_error(cobalt::use_op, ec));
        if (ec) {
            if (ec == boost::system::errc::too_many_files_open ||
                ec == boost::system::errc::too_many_files_open_in_system)
            {
                using namespace std::chrono_literals;
                net::steady_timer retry_timer(executor);
                retry_timer.expires_after(100ms);
                co_await retry_timer.async_wait(boost::asio::redirect_error(cobalt::use_op, ec));
                if (ec)
                    break;

                ec = {};
                continue; // retry accept
            }
            break;
        }
        handle_accept(std::move(sock), net::executor_arg, ex_);
    }
    get_logger()->trace("async_accept: {}", ec.message());
}

cobalt::detached http_server_impl::handle_accept(tcp::socket sock,
                                                 boost::asio::executor_arg_t,
                                                 cobalt::executor)
{
    auto remote_endp = sock.remote_endpoint();
    auto local_endp  = sock.local_endpoint();
    get_logger()->trace(
        "accept new connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());

    auto conn = std::make_shared<session>(std::move(sock), *this);
    {
        std::lock_guard<std::mutex> lck(session_mutex_);
        session_map_.insert(conn);
    }
    try {
        co_await boost::asio::co_spawn(
            co_await cobalt::this_coro::executor, conn->run(), cobalt::use_op);
    }
    catch (const std::exception& e) {
        get_logger()->error("session::run() exception: {}", e.what());
    }
    catch (...) {
        get_logger()->error("session::run() unknown exception");
    }
    {
        std::lock_guard<std::mutex> lck(session_mutex_);
        session_map_.erase(conn);
    }
    get_logger()->trace(
        "close connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());
}

void http_server_impl::set_read_timeout(const std::chrono::steady_clock::duration& dur)
{
    read_timeout_ = dur;
}

void http_server_impl::set_write_timeout(const std::chrono::steady_clock::duration& dur)
{
    write_timeout_ = dur;
}

const std::chrono::steady_clock::duration& http_server_impl::read_timeout() const
{
    return read_timeout_;
}

const std::chrono::steady_clock::duration& http_server_impl::write_timeout() const
{
    return write_timeout_;
}

tcp::endpoint http_server_impl::local_endpoint() const
{
    boost::system::error_code ec;
    return acceptor_.local_endpoint(ec);
}

std::shared_ptr<spdlog::logger> http_server_impl::get_logger() const
{
    if (custom_logger_)
        return custom_logger_;
    return default_logger_;
}

void http_server_impl::set_logger(std::shared_ptr<spdlog::logger> logger)
{
    custom_logger_ = logger;
}

void http_server_impl::use_ssl(const net::const_buffer& cert_file,
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


} // namespace httplib::server