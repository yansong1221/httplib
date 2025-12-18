#include "server_impl.h"
#include "httplib/util/use_awaitable.hpp"
#include "httplib/util/when_all.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#ifdef HTTPLIB_ENABLED_SSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

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
    net::co_spawn(
        ex_,
        [this]() -> net::awaitable<void> { co_await co_run(); },
        [](std::exception_ptr ex) {
            // if an exception occurred in the coroutine,
            // it's something critical, e.g. out of memory
            // we capture normal errors in the ec
            // so we just rethrow the exception here,
            // which will cause `ioc.run()` to throw
            if (ex)
                std::rethrow_exception(ex);
        });
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

net::awaitable<boost::system::error_code> http_server_impl::co_run()
{
    std::vector<net::awaitable<boost::system::error_code>> ops;
    for (int i = 0; i < 32; ++i)
        ops.push_back(co_accept());

    auto&& results = co_await util::when_all(std::move(ops));
    {
        std::lock_guard lck(session_mutex_);
        for (const auto& v : session_map_)
            v->abort();
    }

    for (const auto& ec : results)
        if (ec)
            co_return ec;

    co_return boost::system::error_code {};
}
net::awaitable<boost::system::error_code> http_server_impl::co_accept()
{
    boost::system::error_code ec;
    for (;;) {
        tcp::socket sock(ex_);
        co_await acceptor_.async_accept(sock, util::net_awaitable[ec]);
        if (ec) {
            if (ec == boost::system::errc::too_many_files_open ||
                ec == boost::system::errc::too_many_files_open_in_system)
            {
                ec = {};
                using namespace std::chrono_literals;
                net::steady_timer retry_timer(ex_);
                retry_timer.expires_after(100ms);
                co_await retry_timer.async_wait(util::net_awaitable[ec]);
                if (!ec)
                    continue;
            }
            break;
        }
        net::co_spawn(ex_, handle_accept(std::move(sock)), net::detached);
    }
    get_logger()->trace("async_accept: {}", ec.message());
    co_return ec;
}
net::awaitable<void> http_server_impl::handle_accept(tcp::socket sock)
{
    auto remote_endp = sock.remote_endpoint();
    auto local_endp  = sock.local_endpoint();
    get_logger()->trace(
        "accept new connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());

    auto conn = std::make_shared<session>(std::move(sock), *this);
    {
        std::lock_guard lck(session_mutex_);
        session_map_.insert(conn);
    }
    try {
        co_await conn->run();
    }
    catch (const std::exception& e) {
        get_logger()->error("session::run() exception: {}", e.what());
    }
    catch (...) {
        get_logger()->error("session::run() unknown exception");
    }
    {
        std::lock_guard lck(session_mutex_);
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
#ifdef HTTPLIB_ENABLED_SSL
    unsigned long ssl_options =
        ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::single_dh_use;

    auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
    ssl_ctx->set_options(ssl_options);

    if (!passwd.empty()) {
        ssl_ctx->set_password_callback([pass = std::move(passwd)](auto, auto) {
            if (pass.empty())
                throw std::runtime_error("ssl password is empty!");
            return pass;
        });
    }
    ssl_ctx->use_certificate(cert_file, ssl::context_base::pem);
    ssl_ctx->use_rsa_private_key(key_file, ssl::context::pem);

    ssl_context_ = ssl_ctx;
#else
    throw boost::system::system_error(
        boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported));
#endif
}


} // namespace httplib::server