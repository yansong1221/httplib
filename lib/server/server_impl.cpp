#include "server_impl.h"
#include "httplib/client/client_pool.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "httplib/util/when_all.hpp"
#include "request_impl.hpp"
#include "response_impl.hpp"
#include <boost/asio/use_future.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#ifdef HTTPLIB_ENABLED_SSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace httplib::server {

http_server::impl::impl(const net::any_io_executor& ex)
    : ex_(ex)
    , acceptor_(ex)
{
    proxy_pool_ = std::make_unique<client::http_client_pool>(ex_);

    auto console_sink                 = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::sinks_init_list sink_list = {console_sink};
    default_logger_ = std::make_shared<spdlog::logger>("httplib.server", sink_list);
    default_logger_->set_level(spdlog::level::info);
}

http_server::impl::~impl() = default;

void http_server::impl::listen(std::string_view host,
                               uint16_t port,
                               int backlog /*= net::socket_base::max_listen_connections*/)
{
    tcp::resolver resolver(ex_);
    auto results = resolver.resolve(host, std::to_string(port));

    tcp::endpoint endp(*results.begin());
    acceptor_.open(endp.protocol());
#ifndef _WIN32
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
#endif
    acceptor_.bind(endp);
    acceptor_.listen(backlog);


    auto listen_endp = local_endpoint();
    logger()->info(
        "Http Server Listen on: [{}:{}]", listen_endp.address().to_string(), listen_endp.port());
}

net::any_io_executor http_server::impl::get_executor() noexcept
{
    return ex_;
}

std::shared_future<boost::system::error_code> http_server::impl::run()
{
    return net::co_spawn(
        ex_,
        [this, self = shared_from_this()]() -> net::awaitable<boost::system::error_code> {
            co_return co_await async_run();
        },
        boost::asio::use_future);
}

std::shared_future<void> http_server::impl::stop()
{
    return net::co_spawn(
        ex_,
        [this, self = shared_from_this()]() -> net::awaitable<void> {
            co_return co_await async_stop();
        },
        boost::asio::use_future);
}
httplib::net::awaitable<void> http_server::impl::async_stop()
{
    if (acceptor_.is_open()) {
        boost::system::error_code ec;
        acceptor_.cancel(ec);
        acceptor_.close(ec);
    }
    {
        std::lock_guard lck(session_mutex_);
        for (const auto& v : sessions_)
            v->abort();
    }
    proxy_pool_->stop();

    boost::system::error_code ec;
    boost::asio::steady_timer wait_timer(ex_);

    while (true) {
        {
            std::lock_guard lck(session_mutex_);
            if (sessions_.empty())
                break;
        }

        wait_timer.expires_after(std::chrono::milliseconds(100));
        co_await wait_timer.async_wait(util::net_awaitable[ec]);
        if (ec)
            break;
    }
}

router_impl& http_server::impl::router()
{
    return router_;
}

net::awaitable<boost::system::error_code> http_server::impl::async_run()
{
    std::vector<net::awaitable<boost::system::error_code>> ops;
    for (int i = 0; i < 32; ++i)
        ops.push_back(co_accept());

    auto&& results = co_await util::when_all(std::move(ops));

    co_await async_stop();

    for (const auto& ec : results)
        if (ec)
            co_return ec;

    co_return boost::system::error_code {};
}
net::awaitable<boost::system::error_code> http_server::impl::co_accept()
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
    logger()->trace("async_accept: {}", ec.message());
    co_return ec;
}
net::awaitable<void> http_server::impl::handle_accept(tcp::socket sock)
{
    auto remote_endp = sock.remote_endpoint();
    auto local_endp  = sock.local_endpoint();
    logger()->trace(
        "accept new connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());

    auto conn = std::make_shared<session>(std::move(sock), *this);
    {
        std::lock_guard lck(session_mutex_);
        sessions_.insert(conn);
    }
    try {
        co_await conn->run();
    }
    catch (const std::exception& e) {
        logger()->error("session::run() exception: {}", e.what());
    }
    catch (...) {
        logger()->error("session::run() unknown exception");
    }
    {
        std::lock_guard lck(session_mutex_);
        sessions_.erase(conn);
    }
    logger()->trace(
        "close connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());
}

void http_server::impl::set_read_timeout(const std::chrono::steady_clock::duration& dur)
{
    read_timeout_ = dur;
}

void http_server::impl::set_write_timeout(const std::chrono::steady_clock::duration& dur)
{
    write_timeout_ = dur;
}

std::chrono::steady_clock::duration http_server::impl::read_timeout() const
{
    return read_timeout_;
}

std::chrono::steady_clock::duration http_server::impl::write_timeout() const
{
    return write_timeout_;
}

tcp::endpoint http_server::impl::local_endpoint() const
{
    boost::system::error_code ec;
    return acceptor_.local_endpoint(ec);
}

std::shared_ptr<spdlog::logger> http_server::impl::logger() const
{
    if (custom_logger_)
        return custom_logger_;
    return default_logger_;
}

void http_server::impl::set_logger(std::shared_ptr<spdlog::logger> logger)
{
    custom_logger_ = logger;
}

void http_server::impl::set_compress_content_types(std::function<bool(std::string_view)> predicate)
{
    compress_content_type_predicate_ = std::move(predicate);
}

static bool default_compress_content_type(std::string_view content_type)
{
    return content_type.starts_with("text/") || content_type.starts_with("application/json") ||
           content_type.starts_with("application/javascript") ||
           content_type.starts_with("application/xml") ||
           content_type.starts_with("application/xhtml+xml") ||
           content_type.starts_with("image/svg+xml") ||
           content_type.starts_with("application/ld+json");
}

bool http_server::impl::should_compress_content_type(std::string_view content_type) const
{
    if (compress_content_type_predicate_)
        return compress_content_type_predicate_(content_type);
    return default_compress_content_type(content_type);
}

void http_server::impl::use_ssl(const net::const_buffer& cert_file,
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

void http_server::impl::set_reverse_proxy(std::string_view key,
                                          std::string_view upstream_host,
                                          uint16_t upstream_port,
                                          bool upstream_ssl)
{
    std::string prefix(key);
    if (prefix.ends_with('*'))
        prefix.pop_back();
    if (prefix.ends_with('/'))
        prefix.pop_back();

    router_.set_buffer_body_http_handler<http::verb::get,
                                         http::verb::head,
                                         http::verb::post,
                                         http::verb::put,
                                         http::verb::patch,
                                         http::verb::delete_,
                                         http::verb::options>(
        key,
        [this, upstream_host = std::string(upstream_host), upstream_port, upstream_ssl, prefix](
            request& req, response& resp) -> net::awaitable<void> {
            auto target = std::string(req.target());
            if (target.starts_with(prefix))
                target = target.substr(prefix.size());
            if (target.empty() || target[0] != '/')
                target.insert(0, 1, '/');

            http::fields upstream_headers;
            auto& req_impl = *req.get_impl();
            for (const auto& f : req_impl) {
                if (f.name() != http::field::host)
                    upstream_headers.set(f.name_string(), f.value());
            }

            auto client_ip = req.get_client_ip().to_string();
            auto xff       = req_impl["X-Forwarded-For"];
            if (!xff.empty()) {
                std::string xf(xff);
                client_ip = xf + ", " + client_ip;
            }
            upstream_headers.set("X-Forwarded-For", client_ip);

            auto client =
                co_await proxy_pool_->async_acquire(upstream_host, upstream_port, upstream_ssl);

            auto rel_ec =
                co_await client->relay().write_header(req.method(), target, upstream_headers);
            if (rel_ec) {
                logger()->warn("relay write_header failed: {}", rel_ec.message());
                resp.set_error_content(http::status::bad_gateway);
                co_return;
            }

            std::array<char, 8192> relay_buf {};
            {
                while (true) {
                    auto bytes = co_await req.read_buffer_body_some(net::buffer(relay_buf));
                    rel_ec =
                        co_await client->relay().write_body(net::buffer(relay_buf, bytes),
                                                            bytes != 0);
                    if (rel_ec) {
                        logger()->warn("relay write_body failed: {}", rel_ec.message());
                        resp.set_error_content(http::status::bad_gateway);
                        co_return;
                    }
                    if (bytes == 0)
                        break;
                }
            }

            rel_ec = co_await client->relay().read_header();
            if (rel_ec) {
                logger()->warn("relay read_header failed: {}", rel_ec.message());
                resp.set_error_content(http::status::bad_gateway);
                co_return;
            }

            const auto& headers = client->relay().headers();
            const auto result   = client->relay().result();

            co_await resp.relay().write_header(result, headers);

            {
                while (true) {
                    auto bytes_result =
                        co_await client->relay().read_body(net::buffer(relay_buf));
                    if (bytes_result.has_error())
                        break;
                    auto bytes = bytes_result.value();
                    co_await resp.relay().write_body(net::buffer(relay_buf, bytes), bytes != 0);
                    if (bytes == 0)
                        break;
                }
            }

            //resp.set_string_content(newbody, headers[httplib::http::field::content_type], result);

            // resp.set_buffer_body_write_handler(
            //     [client = std::move(client), relay_buf = std::move(relay_buf)](
            //         httplib::streaming::buffer_body_writer& writer) mutable
            //         -> net::awaitable<void> {
            //         while (true) {
            //             auto bytes = co_await client->relay().read_body(net::buffer(relay_buf));
            //             co_await writer.write(net::buffer(relay_buf, bytes), bytes != 0);
            //             if (bytes == 0)
            //                 break;
            //         }
            //     },
            //     headers,
            //     result);
        });
}


} // namespace httplib::server