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
#include "util/logging.hpp"
#include "ws_forward_impl.h"
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <spdlog/spdlog.h>

#ifdef HTTPLIB_ENABLED_SSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace httplib::server
{
    namespace detail
    {
        static std::string
        read_file_fast(fs::path const& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
            {
                throw std::runtime_error("Cannot open file: " + path.string());
            }

            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);

            std::string buffer(size, 0);
            if (!file.read(buffer.data(), size))
            {
                throw std::runtime_error("Failed to read file: " + path.string());
            }

            return buffer;
        }
    } // namespace detail

    http_server::impl::impl(net::any_io_executor const& ex)
        : ex_(ex)
        , acceptor_(ex)
        , httplib::detail::logger("httplib.server")
    {
    }

    http_server::impl::~impl() = default;

    void
    http_server::impl::listen(std::string_view host, uint16_t port)
    {
        tcp::resolver resolver(ex_);
        auto results = resolver.resolve(host, std::to_string(port));

        tcp::endpoint endp(*results.begin());
        acceptor_.open(endp.protocol());
#ifndef _WIN32
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
#endif
        acceptor_.bind(endp);
        acceptor_.listen(net::socket_base::max_listen_connections);

        boost::system::error_code ec;
        local_endpoint_ = acceptor_.local_endpoint(ec);
        get_logger()->info("Http Server Listen on: [{}:{}]",
                           local_endpoint_.address().to_string(),
                           local_endpoint_.port());
    }

    net::any_io_executor
    http_server::impl::get_executor() noexcept
    {
        return ex_;
    }

    std::future<boost::system::error_code>
    http_server::impl::run()
    {
        return net::co_spawn(
            ex_,
            [self = shared_from_this(), this]() -> net::awaitable<boost::system::error_code>
            { co_return co_await async_run(); },
            boost::asio::use_future);
    }

    std::future<void>
    http_server::impl::stop()
    {
        return net::co_spawn(
            ex_,
            [self = shared_from_this(), this]() -> net::awaitable<void>
            {
                co_await async_stop();
                co_return;
            },
            boost::asio::use_future);
    }
    httplib::net::awaitable<void>
    http_server::impl::async_stop()
    {
        auto self = shared_from_this();
        co_return co_await net::co_spawn(
            ex_,
            [&]() -> net::awaitable<void>
            {
                if (!running_)
                {
                    co_return;
                }

                boost::system::error_code ec;
                acceptor_.cancel(ec);
                acceptor_.close(ec);

                // 先在锁内快照，避免持锁调用 abort()/net::post 造成阻塞或重入。
                std::vector<std::shared_ptr<session>> sessions;
                {
                    std::lock_guard lck(session_mutex_);
                    sessions.assign(sessions_.begin(), sessions_.end());
                }
                get_logger()->trace("[server] stopping, {} sessions remaining", sessions.size());
                for (auto const& v : sessions)
                {
                    v->abort();
                }

                // `async_run()` closes `stop_event_` on exit, so every caller observes
                // completion without polling.
                (void)co_await stop_event_.wait();
            },
            net::use_awaitable);
    }

    router_impl&
    http_server::impl::router()
    {
        return router_;
    }

    net::awaitable<boost::system::error_code>
    http_server::impl::async_run()
    {
        auto self = shared_from_this();
        co_return co_await net::co_spawn(
            ex_,
            [&]() -> net::awaitable<boost::system::error_code>
            {
                if (running_.exchange(true))
                {
                    co_return boost::asio::error::make_error_code(boost::asio::error::already_started);
                }
                // Reopen the completion event so the instance can be run again after a
                // previous stop()/async_stop() closed it.
                stop_event_.reset();
                session_event_.reset();

                std::vector<net::awaitable<boost::system::error_code>> ops;
                for (int i = 0; i < acceptor_count_; ++i)
                {
                    ops.push_back(co_accept());
                }

                auto&& results = co_await util::when_all(std::move(ops));

                // stop();

                // Wait for every in-flight session to finish. `handle_accept()` notifies
                // `session_event_` when the last session leaves `sessions_`, so no timer
                // polling is needed.
                for (;;)
                {
                    {
                        std::lock_guard lck(session_mutex_);
                        if (sessions_.empty())
                        {
                            break;
                        }
                    }

                    auto result = co_await session_event_.wait();
                    if (result != util::async_event::wait_result::notified)
                    {
                        break;
                    }
                }

                router_.reset();
                running_ = false;
                session_event_.close();
                stop_event_.close();
                for (auto const& ec : results)
                {
                    if (ec)
                    {
                        co_return ec;
                    }
                }
                co_return boost::system::error_code {};
            },
            net::use_awaitable);
    }
    net::awaitable<boost::system::error_code>
    http_server::impl::co_accept()
    {
        boost::system::error_code ec;
        for (;;)
        {
            // 每条连接一个 strand：socket/stream 与 abort 都绑定到它，连接内串行执行。
            auto strand = net::make_strand(ex_);
            tcp::socket sock(strand);
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
            net::co_spawn(strand,
                          handle_accept(std::move(sock)),
                          [self = shared_from_this()](std::exception_ptr e)
                          {
                              if (!e)
                              {
                                  return;
                              }
                              try
                              {
                                  std::rethrow_exception(e);
                              }
                              catch (std::exception const& ex)
                              {
                                  self->get_logger()->error("handle_accept exception: {}", ex.what());
                              }
                              catch (...)
                              {
                                  self->get_logger()->error("handle_accept unknown exception");
                              }
                          });
        }
        get_logger()->trace("async_accept: {}", ec.message());
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
        get_logger()->trace("accept new connection [{}:{}]", remote_endp.address().to_string(), remote_endp.port());

        auto conn = std::make_shared<session>(co_await net::this_coro::executor, std::move(sock), shared_from_this());
        std::size_t session_count = 0;
        {
            std::lock_guard lck(session_mutex_);
            sessions_.insert(conn);
            session_count = sessions_.size();
        }
        get_logger()->trace("[session] running, total={}", session_count);
        try
        {
            co_await conn->run();
        }
        catch (std::exception const& e)
        {
            get_logger()->error("session::run() exception: {}", e.what());
        }
        catch (...)
        {
            get_logger()->error("session::run() unknown exception");
        }
        {
            std::lock_guard lck(session_mutex_);
            sessions_.erase(conn);
            session_count = sessions_.size();
        }
        get_logger()->trace("[session] done, total={}", session_count);

        if (session_count == 0)
        {
            session_event_.notify_all();
        }
    }

    void
    http_server::impl::set_read_timeout(std::chrono::steady_clock::duration const& dur)
    {
        read_timeout_.store(dur);
    }

    void
    http_server::impl::set_write_timeout(std::chrono::steady_clock::duration const& dur)
    {
        write_timeout_.store(dur);
    }

    std::chrono::steady_clock::duration
    http_server::impl::read_timeout() const
    {
        return read_timeout_.load();
    }

    std::chrono::steady_clock::duration
    http_server::impl::write_timeout() const
    {
        return write_timeout_.load();
    }

    tcp::endpoint const&
    http_server::impl::local_endpoint() const
    {
        return local_endpoint_;
    }

    void
    http_server::impl::set_compress_content_types(http_server::compress_predicate predicate)
    {
        compress_predicate_.store(std::make_shared<http_server::compress_predicate>(std::move(predicate)));
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
        if (auto predicate = compress_predicate_.load())
        {
            return (*predicate)(content_type);
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

        ssl_context_.store(std::move(ssl_ctx));
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
        set_reverse_proxy(location,
                          std::make_shared<detail::static_upstream_provider>(std::string(upstream_url)),
                          std::move(factory));
    }

    void
    http_server::impl::set_reverse_proxy(std::string_view location,
                                         std::vector<upstream_backend> backends,
                                         upstream_locator locator,
                                         http_server::proxy_interceptor_factory factory)
    {
        auto group = std::make_shared<upstream_group>(make_backends(backends), locator);
        set_reverse_proxy(location, group, std::move(factory));
    }

    void
    http_server::impl::set_reverse_proxy(std::string_view location,
                                         std::shared_ptr<upstream_provider> provider,
                                         http_server::proxy_interceptor_factory factory)
    {
        auto proxy_pool = std::make_shared<client::http_client_pool>(ex_);

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
             provider = std::move(provider),
             factory = std::move(factory)](request& req, response& resp) -> net::awaitable<void>
            {
                detail::reverse_proxy_context ctx(proxy_pool, prefix, provider, factory, get_logger());
                co_await ctx.run(req, resp);
            });
    }

    void
    http_server::impl::set_ws_forward(std::string_view location,
                                      std::string_view upstream_url,
                                      http_server::ws_interceptor_factory factory)
    {
        set_ws_forward(location,
                       std::make_shared<detail::static_upstream_provider>(std::string(upstream_url)),
                       std::move(factory));
    }

    void
    http_server::impl::set_ws_forward(std::string_view location,
                                      std::vector<upstream_backend> backends,
                                      upstream_locator locator,
                                      http_server::ws_interceptor_factory factory)
    {
        auto group = std::make_shared<upstream_group>(make_backends(backends), locator);
        set_ws_forward(location, group, std::move(factory));
    }

    void
    http_server::impl::set_ws_forward(std::string_view location,
                                      std::shared_ptr<upstream_provider> provider,
                                      http_server::ws_interceptor_factory factory)
    {
        std::string prefix = detail::strip_proxy_prefix(location);
        auto logger = this->get_logger();

        router_.set_ws_handler(
            location,
            [ex = ex_, prefix, logger, provider = std::move(provider), factory = std::move(factory)](
                websocket_conn::weak_ptr wp) -> net::awaitable<void>
            {
                detail::ws_forward_context ctx(ex, prefix, provider, factory, logger);
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

    http_server::http_server(net::io_context& ioc) : http_server(ioc.get_executor()) {}

    http_server::http_server(net::any_io_executor const& ex) : impl_(std::make_shared<impl>(ex)) {}

    http_server::~http_server() { stop(); }

    net::any_io_executor
    http_server::get_executor() noexcept
    {
        return impl_->get_executor();
    }

    http_server&
    http_server::listen(std::string_view host, uint16_t port)
    {
        impl_->listen(host, port);
        return *this;
    }

    http_server&
    http_server::listen(uint16_t port)
    {
        return listen("0.0.0.0", port);
    }

    net::awaitable<boost::system::error_code>
    http_server::async_run()
    {
        co_return co_await impl_->async_run();
    }

    std::future<boost::system::error_code>
    http_server::run()
    {
        return impl_->run();
    }

    std::future<void>
    http_server::stop()
    {
        return impl_->stop();
    }
    router&
    http_server::router()
    {
        return impl_->router();
    }

    tcp::endpoint const&
    http_server::local_endpoint() const
    {
        return impl_->local_endpoint();
    }

    void
    http_server::set_read_timeout(std::chrono::steady_clock::duration const& dur)
    {
        impl_->set_read_timeout(dur);
    }

    void
    http_server::set_write_timeout(std::chrono::steady_clock::duration const& dur)
    {
        impl_->set_write_timeout(dur);
    }

    std::chrono::steady_clock::duration
    http_server::read_timeout() const
    {
        return impl_->read_timeout();
    }

    std::chrono::steady_clock::duration
    http_server::write_timeout() const
    {
        return impl_->write_timeout();
    }

    std::shared_ptr<spdlog::logger>
    http_server::logger() const
    {
        return impl_->get_logger();
    }
    void
    http_server::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        impl_->set_logger(logger);
    }

    void
    http_server::set_compress_content_types(compress_predicate predicate)
    {
        impl_->set_compress_content_types(std::move(predicate));
    }

    void
    http_server::set_form_data_config(html::form_data::param const& params)
    {
        impl_->set_form_data_params(params);
    }

    void
    http_server::set_header_limit(std::uint32_t limit)
    {
        impl_->set_header_limit(limit);
    }

    void
    http_server::set_body_limit(std::uint64_t limit)
    {
        impl_->set_body_limit(limit);
    }

    void
    http_server::set_reverse_proxy(std::string_view location, std::string_view url, proxy_interceptor_factory factory)
    {
        impl_->set_reverse_proxy(location, url, std::move(factory));
    }

    void
    http_server::set_reverse_proxy(std::string_view location,
                                   std::shared_ptr<upstream_provider> provider,
                                   proxy_interceptor_factory factory)
    {
        impl_->set_reverse_proxy(location, std::move(provider), std::move(factory));
    }

    void
    http_server::set_reverse_proxy(std::string_view location,
                                   std::vector<upstream_backend> backends,
                                   upstream_locator locator,
                                   proxy_interceptor_factory factory)
    {
        impl_->set_reverse_proxy(location, std::move(backends), locator, std::move(factory));
    }

    void
    http_server::set_ws_forward(std::string_view location, std::string_view url, ws_interceptor_factory factory)
    {
        impl_->set_ws_forward(location, url, std::move(factory));
    }

    void
    http_server::set_ws_forward(std::string_view location,
                                std::shared_ptr<upstream_provider> provider,
                                ws_interceptor_factory factory)
    {
        impl_->set_ws_forward(location, std::move(provider), std::move(factory));
    }

    void
    http_server::set_ws_forward(std::string_view location,
                                std::vector<upstream_backend> backends,
                                upstream_locator locator,
                                ws_interceptor_factory factory)
    {
        impl_->set_ws_forward(location, std::move(backends), locator, std::move(factory));
    }

    void
    http_server::set_ssl(std::span<char const> const& cert_file,
                         std::span<char const> const& key_file,
                         std::string passwd /*= {}*/)
    {
        impl_->use_ssl(cert_file, key_file, passwd);
    }

    void
    http_server::set_ssl_file(fs::path const& cert_file, fs::path const& key_file, std::string passwd /*= {}*/)
    {
        set_ssl(detail::read_file_fast(cert_file), detail::read_file_fast(key_file), passwd);
    }

    net::awaitable<void>
    http_server::async_stop()
    {
        co_return co_await impl_->async_stop();
    }
} // namespace httplib::server
