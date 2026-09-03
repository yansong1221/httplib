#include "server_impl.h"
#include "client/client_impl.h"
#include "httplib/client/client.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/lazy_request.hpp"
#include "httplib/server/proxy_strategy.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "httplib/util/when_all.hpp"
#include "proxy_util.hpp"
#include "request_impl.hpp"
#include "response_impl.hpp"
#include "reverse_proxy_impl.h"
#include "upstream_group.hpp"
#include "ws_forward_impl.h"
#include "util/logging.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/use_future.hpp>
#include <spdlog/spdlog.h>

#ifdef HTTPLIB_ENABLED_SSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace httplib::server
{
    http_server::impl::impl(net::any_io_executor const& ex) : ex_(ex), acceptor_(ex)
    {
        default_logger_ = httplib::detail::make_console_logger("httplib.server");
    }

    http_server::impl::~impl() = default;

    void
    http_server::impl::listen(std::string_view host,
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
        logger()->info("Http Server Listen on: [{}:{}]", listen_endp.address().to_string(), listen_endp.port());
    }

    net::any_io_executor
    http_server::impl::get_executor() noexcept
    {
        return ex_;
    }

    std::shared_future<boost::system::error_code>
    http_server::impl::run()
    {
        return net::co_spawn(
            ex_,
            [self = shared_from_this(), this]() -> net::awaitable<boost::system::error_code>
            { co_return co_await async_run(); },
            boost::asio::use_future);
    }

    void
    http_server::impl::stop()
    {
        if (acceptor_.is_open())
        {
            boost::system::error_code ec;
            acceptor_.cancel(ec);
            acceptor_.close(ec);
        }
        {
            std::lock_guard lck(session_mutex_);
            auto count = sessions_.size();
            logger()->trace("[server] stopping, {} sessions remaining", count);
            for (auto const& v : sessions_)
            {
                v->abort();
            }
        }
    }
    httplib::net::awaitable<void>
    http_server::impl::async_stop()
    {
        stop();

        boost::system::error_code ec;
        boost::asio::steady_timer wait_timer(ex_);

        while (running_)
        {
            wait_timer.expires_after(std::chrono::milliseconds(100));
            co_await wait_timer.async_wait(util::net_awaitable[ec]);
            if (ec)
            {
                break;
            }
        }
    }

    router_impl&
    http_server::impl::router()
    {
        return router_;
    }

    net::awaitable<boost::system::error_code>
    http_server::impl::async_run()
    {
        if (running_.exchange(true))
        {
            co_return boost::asio::error::make_error_code(boost::asio::error::already_started);
        }

        std::vector<net::awaitable<boost::system::error_code>> ops;
        for (int i = 0; i < acceptor_count_; ++i)
        {
            ops.push_back(co_accept());
        }

        auto&& results = co_await util::when_all(std::move(ops));

        // stop();

        boost::system::error_code ec;
        boost::asio::steady_timer wait_timer(ex_);
        while (true)
        {
            {
                std::lock_guard lck(session_mutex_);
                if (sessions_.empty())
                {
                    break;
                }
            }

            wait_timer.expires_after(std::chrono::milliseconds(100));
            co_await wait_timer.async_wait(util::net_awaitable[ec]);
            if (ec)
            {
                break;
            }
        }

        router_.reset();
        running_ = false;
        for (auto const& ec : results)
        {
            if (ec)
            {
                co_return ec;
            }
        }
        co_return boost::system::error_code {};
    }
    net::awaitable<boost::system::error_code>
    http_server::impl::co_accept()
    {
        boost::system::error_code ec;
        for (;;)
        {
            tcp::socket sock(ex_);
            co_await acceptor_.async_accept(sock, util::net_awaitable[ec]);
            if (ec)
            {
                if (ec == boost::system::errc::too_many_files_open
                    || ec == boost::system::errc::too_many_files_open_in_system)
                {
                    ec = {};
                    using namespace std::chrono_literals;
                    net::steady_timer retry_timer(ex_);
                    retry_timer.expires_after(100ms);
                    co_await retry_timer.async_wait(util::net_awaitable[ec]);
                    if (!ec)
                    {
                        continue;
                    }
                }
                break;
            }
            net::co_spawn(ex_,
                          handle_accept(std::move(sock)),
                          [](std::exception_ptr e)
                          {
                              if (e)
                              {
                                  std::rethrow_exception(e);
                              }
                          });
        }
        logger()->trace("async_accept: {}", ec.message());
        co_return ec;
    }
    net::awaitable<void>
    http_server::impl::handle_accept(tcp::socket sock)
    {
        auto remote_endp = sock.remote_endpoint();
        auto local_endp = sock.local_endpoint();
        {
            boost::system::error_code ec;
            sock.set_option(net::ip::tcp::no_delay(true), ec);
        }
        logger()->trace("accept new connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());

        auto conn = std::make_shared<session>(std::move(sock), shared_from_this());
        {
            std::lock_guard lck(session_mutex_);
            sessions_.insert(conn);
        }
        logger()->trace("[session] running, total={}", sessions_.size());
        try
        {
            co_await conn->run();
        }
        catch (std::exception const& e)
        {
            logger()->error("session::run() exception: {}", e.what());
        }
        catch (...)
        {
            logger()->error("session::run() unknown exception");
        }
        {
            std::lock_guard lck(session_mutex_);
            sessions_.erase(conn);
        }
        logger()->trace("[session] done, total={}", sessions_.size());
    }

    void
    http_server::impl::set_read_timeout(std::chrono::steady_clock::duration const& dur)
    {
        read_timeout_ = dur;
    }

    void
    http_server::impl::set_write_timeout(std::chrono::steady_clock::duration const& dur)
    {
        write_timeout_ = dur;
    }

    std::chrono::steady_clock::duration
    http_server::impl::read_timeout() const
    {
        return read_timeout_;
    }

    std::chrono::steady_clock::duration
    http_server::impl::write_timeout() const
    {
        return write_timeout_;
    }

    int
    http_server::impl::acceptor_count() const
    {
        return acceptor_count_;
    }
    void
    http_server::impl::set_acceptor_count(int n)
    {
        acceptor_count_ = n;
    }
    int
    http_server::impl::proxy_buffer_size() const
    {
        return proxy_buffer_size_;
    }
    void
    http_server::impl::set_proxy_buffer_size(int sz)
    {
        proxy_buffer_size_ = sz;
    }

    tcp::endpoint
    http_server::impl::local_endpoint() const
    {
        boost::system::error_code ec;
        return acceptor_.local_endpoint(ec);
    }

    std::shared_ptr<spdlog::logger>
    http_server::impl::logger() const
    {
        if (custom_logger_)
        {
            return custom_logger_;
        }
        return default_logger_;
    }

    void
    http_server::impl::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        custom_logger_ = logger;
    }

    void
    http_server::impl::set_compress_content_types(std::function<bool(std::string_view)> predicate)
    {
        compress_content_type_predicate_ = std::move(predicate);
    }

    static bool
    default_compress_content_type(std::string_view content_type)
    {
        return content_type.starts_with("text/") || content_type.starts_with("application/json")
               || content_type.starts_with("application/javascript") || content_type.starts_with("application/xml")
               || content_type.starts_with("application/xhtml+xml") || content_type.starts_with("image/svg+xml")
               || content_type.starts_with("application/ld+json");
    }

    bool
    http_server::impl::should_compress_content_type(std::string_view content_type) const
    {
        if (compress_content_type_predicate_)
        {
            return compress_content_type_predicate_(content_type);
        }
        return default_compress_content_type(content_type);
    }

    void
    http_server::impl::use_ssl(net::const_buffer const& cert_file,
                               net::const_buffer const& key_file,
                               std::string passwd /*= {}*/)
    {
#ifdef HTTPLIB_ENABLED_SSL
        unsigned long ssl_options
            = ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::single_dh_use;

        auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
        ssl_ctx->set_options(ssl_options);

        if (!passwd.empty())
        {
            ssl_ctx->set_password_callback(
                [pass = std::move(passwd)](auto, auto)
                {
                    if (pass.empty())
                    {
                        throw std::runtime_error("ssl password is empty!");
                    }
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

    void
    http_server::impl::set_reverse_proxy(std::string_view location,
                                         std::string_view upstream_url,
                                         http_server::proxy_interceptor_factory factory)
    {
        set_reverse_proxy(location, detail::make_static_resolver(std::string(upstream_url)), std::move(factory));
    }

    void
    http_server::impl::set_reverse_proxy(std::string_view location,
                                         std::vector<upstream_backend> backends,
                                         upstream_locator locator,
                                         http_server::proxy_interceptor_factory factory)
    {
        auto group = std::make_shared<upstream_group>(make_backends(backends), locator);
        set_reverse_proxy(
            location,
            [g = std::move(group)](request&) -> net::awaitable<std::shared_ptr<http_server::proxy_target>>
            { co_return g->resolve_target(); },
            std::move(factory));
    }

    void
    http_server::impl::set_reverse_proxy(std::string_view location,
                                         http_server::proxy_resolver resolver,
                                         http_server::proxy_interceptor_factory factory)
    {

        auto proxy_pool = std::make_shared<client::http_client_pool>(ex_);
        proxy_pool->start();

        std::string prefix = detail::strip_proxy_prefix(location);

        router_.set_lazy_http_handler<http::verb::get,
                                      http::verb::head,
                                      http::verb::post,
                                      http::verb::put,
                                      http::verb::patch,
                                      http::verb::delete_,
                                      http::verb::options>(
            location,
            [this,
             self = shared_from_this(),
             proxy_pool,
             prefix,
             resolver = std::move(resolver),
             factory = std::move(factory)](request& req, response& resp) -> net::awaitable<void>
            {
                detail::reverse_proxy_context ctx(proxy_pool, prefix, resolver, factory, logger());
                co_await ctx.run(req, resp);
            });
    }

    void
    http_server::impl::set_ws_forward(std::string_view location,
                                      std::string_view upstream_url,
                                      http_server::ws_interceptor_factory factory)
    {
        set_ws_forward(location, detail::make_static_resolver(std::string(upstream_url)), std::move(factory));
    }

    void
    http_server::impl::set_ws_forward(std::string_view location,
                                      http_server::proxy_resolver resolver,
                                      http_server::ws_interceptor_factory factory)
    {
        std::string prefix = detail::strip_proxy_prefix(location);
        auto logger = this->logger();

        router_.set_ws_handler(
            location,
            [ex = ex_, prefix, logger, resolver = std::move(resolver), factory = std::move(factory)](
                websocket_conn::weak_ptr wp) -> net::awaitable<void>
            {
                detail::ws_forward_context ctx(ex, prefix, resolver, factory, logger);
                co_await ctx.run(wp);
            },
            [](websocket_conn::weak_ptr wp, std::string_view data, bool binary) -> net::awaitable<void>
            { co_await detail::ws_forward_context::send_to_upstream(wp, data, binary); },
            [](websocket_conn::weak_ptr wp) -> net::awaitable<void>
            { co_await detail::ws_forward_context::close_upstream(wp); });
    }

    bool
    http_server::impl::is_open() const
    {
        return acceptor_.is_open();
    }

} // namespace httplib::server
