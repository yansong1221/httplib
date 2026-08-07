#include "server_impl.h"
#include "httplib/client/client.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/read_session.hpp"
#include "httplib/client/write_session.hpp"
#include "httplib/client/ws_client.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "httplib/util/when_all.hpp"
#include "request_impl.hpp"
#include "response_impl.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/url.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#ifdef HTTPLIB_ENABLED_SSL
#    include <openssl/err.h>
#    include <openssl/ssl.h>
#endif

namespace httplib::server
{
    namespace detail
    {

        constexpr auto kWsForwardKey = "ws-forward";
        constexpr auto kWsInterceptorKey = "ws-interceptor";
        using ws_client_ptr = std::shared_ptr<client::ws_client>;
        using ws_interceptor_ptr = std::shared_ptr<ws_interceptor>;

        static std::string
        strip_proxy_prefix(std::string_view route)
        {
            std::string result(route);
            while (!result.empty() && (result.back() == '/' || result.back() == '*'))
            {
                result.pop_back();
            }
            return result;
        }

        static std::string
        make_upstream_path(std::string_view client_target,
                           std::string_view proxy_prefix,
                           std::string_view upstream_base)
        {
            if (!client_target.starts_with(proxy_prefix))
            {
                return {};
            }

            auto tail = client_target.substr(proxy_prefix.size());

            if (tail.empty())
            {
                return upstream_base.empty() ? "/" : std::string(upstream_base);
            }

            std::string result(upstream_base);

            if (result.size() > 1 && result.ends_with('/'))
            {
                result.pop_back();
            }

            if (tail.front() != '/' && tail.front() != '?')
            {
                result += '/';
            }
            else if (result.ends_with('/') && tail.front() == '/')
            {
                tail.remove_prefix(1);
            }

            result += tail;

            return result;
        }

        static http::status
        upstream_error_to_status(boost::system::error_code ec)
        {
            if (ec == boost::asio::error::timed_out || ec == boost::beast::error::timeout)
            {
                return http::status::gateway_timeout;
            }
            return http::status::bad_gateway;
        }

    } // namespace detail

    http_server::impl::impl(net::any_io_executor const& ex) : ex_(ex), acceptor_(ex)
    {
        proxy_pool_ = std::make_unique<client::http_client_pool>(ex_);

        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        spdlog::sinks_init_list sink_list = { console_sink };
        default_logger_ = std::make_shared<spdlog::logger>("httplib.server", sink_list);
        default_logger_->set_level(spdlog::level::info);
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
        if (!is_open())
        {
            return;
        }

        if (shutting_down_.exchange(true))
        {
            return;
        }
        if (acceptor_.is_open())
        {
            boost::system::error_code ec;
            acceptor_.cancel(ec);
            acceptor_.close(ec);
        }
        proxy_pool_->stop();
        {
            std::lock_guard lck(session_mutex_);
            auto count = sessions_.size();
            logger()->info("[server] stopping, {} sessions remaining", count);
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
    }

    router_impl&
    http_server::impl::router()
    {
        return router_;
    }

    net::awaitable<boost::system::error_code>
    http_server::impl::async_run()
    {
        shutting_down_ = false;

        std::vector<net::awaitable<boost::system::error_code>> ops;
        for (int i = 0; i < acceptor_count_; ++i)
        {
            ops.push_back(co_accept());
        }

        auto&& results = co_await util::when_all(std::move(ops));

        co_await async_stop();

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
            net::co_spawn(ex_, handle_accept(std::move(sock)), net::detached);
        }
        logger()->trace("async_accept: {}", ec.message());
        co_return ec;
    }
    net::awaitable<void>
    http_server::impl::handle_accept(tcp::socket sock)
    {
        auto remote_endp = sock.remote_endpoint();
        auto local_endp = sock.local_endpoint();
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
        auto u = std::string(upstream_url);
        set_reverse_proxy(
            location,
            [u = std::move(u)](request&) -> net::awaitable<std::string> { co_return u; },
            std::move(factory));
    }

    void
    http_server::impl::set_reverse_proxy(std::string_view location,
                                         http_server::proxy_resolver resolver,
                                         http_server::proxy_interceptor_factory factory)
    {
        std::string prefix = detail::strip_proxy_prefix(location);

        router_.set_chunked_http_handler<http::verb::get,
                                         http::verb::head,
                                         http::verb::post,
                                         http::verb::put,
                                         http::verb::patch,
                                         http::verb::delete_,
                                         http::verb::options>(
            location,
            [self = shared_from_this(), this, prefix, resolver = std::move(resolver), factory = std::move(factory)](
                request& req,
                response& resp) -> net::awaitable<void>
            {
                auto interceptor = factory ? factory(req) : nullptr;

                auto url = co_await resolver(req);
                logger()->debug("[proxy] {} {} -> {}", req.method_string(), req.target(), url);

                auto r = boost::urls::parse_uri(url);
                if (!r)
                {
                    logger()->trace("[proxy] invalid upstream url: {}", url);
                    resp.set_error_content(http::status::bad_gateway);
                    co_return;
                }

                auto const& u = *r;
                auto host = u.host();
                auto port = (u.scheme_id() == boost::urls::scheme::https ? 443 : 80);
                port = u.has_port() ? u.port_number() : port;
                auto ssl = u.scheme_id() == boost::urls::scheme::https;
                std::string upstream_prefix(u.encoded_path());
                auto upstream_target = detail::make_upstream_path(req.target(), prefix, upstream_prefix);
                auto upstream_url = util::make_url_value(u.host(), port, ssl, upstream_target);

                http::fields upstream_headers(req.base());

                // Strip and rewrite Cookie Domain/Path before forwarding upstream
                if (auto cookie = req[http::field::cookie]; !cookie.empty())
                {
                    std::vector<std::string> rewritten;
                    for (auto item : util::split(cookie, ";"))
                    {
                        auto parts = util::split(item, "=");
                        if (parts.empty())
                        {
                            continue;
                        }
                        auto key = parts.front();
                        if (boost::iequals(key, "Domain") || boost::iequals(key, "Path"))
                        {
                            continue;
                        }
                        rewritten.emplace_back(item);
                    }
                    using namespace std::string_view_literals;
                    rewritten.emplace_back(std::format("Domain={}", util::make_host_value(u.host(), port, ssl)));
                    rewritten.emplace_back(std::format("Path={}", upstream_prefix.empty() ? "/"sv : upstream_prefix));
                    upstream_headers.set(http::field::cookie, boost::join(rewritten, ";"));
                }

                auto client_ip = req.remote_endpoint().address().to_string();
                if (auto xff = req["X-Forwarded-For"]; !xff.empty())
                {
                    client_ip = std::format("{},{}", xff, client_ip);
                }
                upstream_headers.set("X-Forwarded-For", client_ip);
                upstream_headers.set("X-Forwarded-Proto", ssl ? "https" : "http");
                upstream_headers.set("X-Forwarded-Host", req["Host"]);

                // Rewrite Referer to upstream
                if (auto ref = req[http::field::referer]; !ref.empty())
                {
                    auto r = boost::urls::parse_uri(ref);
                    if (r)
                    {
                        auto ref_path = std::string(r->encoded_path());
                        if (ref_path.starts_with(prefix))
                        {
                            ref_path = ref_path.substr(prefix.size());
                        }
                        if (!upstream_prefix.empty())
                        {
                            ref_path.insert(0, upstream_prefix);
                        }
                        if (ref_path.empty() || ref_path[0] != '/')
                        {
                            ref_path.insert(0, 1, '/');
                        }

                        auto new_ref = std::format("{}://{}{}",
                                                   u.scheme(),
                                                   util::make_host_value(u.host(), port, ssl),
                                                   ref_path);
                        if (!r->encoded_query().empty())
                        {
                            new_ref += std::format("?{}", std::string(r->encoded_query()));
                        }
                        if (!r->encoded_fragment().empty())
                        {
                            new_ref += std::format("#{}", std::string(r->encoded_fragment()));
                        }
                        upstream_headers.set(http::field::referer, new_ref);
                    }
                }

                if (interceptor)
                {
                    co_await interceptor->on_upstream_request(req, upstream_headers, upstream_url);
                }

                auto client = co_await proxy_pool_->async_acquire(host, port, ssl);
                if (!client)
                {
                    logger()->trace("[proxy] acquire client failed for {}:{}", host, port);
                    resp.set_error_content(http::status::service_unavailable);
                    co_return;
                }

                auto writer = client->create_writer();
                auto reader = client->create_reader();

                if (auto rel_ec = co_await writer->write_header(req.method(), upstream_target, upstream_headers);
                    rel_ec)
                {
                    logger()->trace("[proxy] write_header to {}:{} failed: {}", host, port, rel_ec.message());
                    resp.set_error_content(detail::upstream_error_to_status(rel_ec));
                    co_return;
                }

                std::array<char, 8192> relay_buf {};

                while (!req.get_chunk_reader()->is_done())
                {
                    auto bytes_result = co_await req.get_chunk_reader()->read_some(net::buffer(relay_buf));
                    if (bytes_result.has_error())
                    {
                        logger()->trace("[proxy] read request body failed: {}", bytes_result.error().message());
                        resp.set_error_content(http::status::bad_request);
                        co_return;
                    }
                    auto more = !req.get_chunk_reader()->is_done();
                    auto bytes = bytes_result.value();

                    if (interceptor)
                    {
                        co_await interceptor->on_upstream_request_body(net::buffer(relay_buf, bytes), more);
                    }

                    if (auto rel_ec = co_await writer->write_body(net::buffer(relay_buf, bytes), more); rel_ec)
                    {
                        logger()->trace("[proxy] write_body to {}:{} failed: {}", host, port, rel_ec.message());
                        resp.set_error_content(detail::upstream_error_to_status(rel_ec));
                        co_return;
                    }
                }

                if (auto rel_ec = co_await reader->read_header(); rel_ec)
                {
                    logger()->trace("[proxy] read_header from {}:{} failed: {}", host, port, rel_ec.message());
                    resp.set_error_content(detail::upstream_error_to_status(rel_ec));
                    co_return;
                }

                auto const& headers = reader->headers();
                auto const result = reader->result();
                logger()->debug("[proxy] {} {} <- {} {}",
                                req.method_string(),
                                req.target(),
                                static_cast<unsigned>(result),
                                u.host());

                if (interceptor)
                {
                    co_await interceptor->on_upstream_response(req, result, headers);
                }

                auto response_hdrs = http::fields(headers);
                // Strip hop-by-hop headers before relaying to client
                response_hdrs.erase(http::field::connection);
                response_hdrs.erase(http::field::keep_alive);
                response_hdrs.erase(http::field::te);
                response_hdrs.erase(http::field::trailer);
                response_hdrs.erase(http::field::upgrade);

                if (result >= http::status::moved_permanently && result <= http::status::permanent_redirect
                    && result != http::status::not_modified)
                {
                    auto upstream_base = util::make_url_value(u.host(), port, ssl);
                    std::string location(response_hdrs[http::field::location]);
                    if (location.starts_with(upstream_base))
                    {
                        response_hdrs.set(http::field::location, prefix + location.substr(upstream_base.size()));
                    }
                }

                if (auto rel_ec = co_await resp.get_chunk_writer()->write_header(result, response_hdrs); rel_ec)
                {
                    logger()->trace("[proxy] write response header failed: {}", rel_ec.message());
                    co_return;
                }

                while (!reader->is_body_done())
                {
                    auto bytes_result = co_await reader->read_body(net::buffer(relay_buf));
                    if (bytes_result.has_error())
                    {
                        logger()->trace("[proxy] read response body failed: {}", bytes_result.error().message());
                        co_return;
                    }
                    auto bytes = bytes_result.value();
                    auto more = !reader->is_body_done();

                    if (interceptor)
                    {
                        co_await interceptor->on_upstream_response_body(net::buffer(relay_buf, bytes), more);
                    }

                    if (auto rel_ec = co_await resp.get_chunk_writer()->write_body(net::buffer(relay_buf, bytes), more);
                        rel_ec)
                    {
                        logger()->trace("[proxy] write response body failed: {}", rel_ec.message());
                        co_return;
                    }
                }
            });
    }

    void
    http_server::impl::set_ws_forward(std::string_view location,
                                      std::string_view upstream_url,
                                      http_server::ws_interceptor_factory factory)
    {
        auto u = std::string(upstream_url);
        set_ws_forward(
            location,
            [u = std::move(u)](request&) -> net::awaitable<std::string> { co_return u; },
            std::move(factory));
    }

    void
    http_server::impl::set_ws_forward(std::string_view location,
                                      http_server::proxy_resolver resolver,
                                      http_server::ws_interceptor_factory factory)
    {
        std::string prefix = detail::strip_proxy_prefix(location);

        auto open_handler
            = [self = shared_from_this(), this, prefix, resolver = std::move(resolver), factory = std::move(factory)](
                  websocket_conn::weak_ptr wp) -> net::awaitable<void>
        {
            auto conn = wp.lock();
            if (!conn)
            {
                co_return;
            }

            auto& req = conn->http_request();
            auto interceptor = factory ? factory(req) : nullptr;

            auto url = co_await resolver(req);

            auto r = boost::urls::parse_uri(url);
            if (!r)
            {
                logger()->warn("[ws-forward] invalid upstream url: {}", url);
                conn->close("bad upstream");
                co_return;
            }

            auto const& u = *r;
            auto host = u.host();
            auto scheme = u.scheme();
            auto ssl = (scheme == "wss" || scheme == "https");
            auto port = u.port_number() ? u.port_number() : (ssl ? 443 : 80);

            auto upstream_target = detail::make_upstream_path(req.target(), prefix, u.encoded_path());
            auto upstream_url = util::make_url_value(host, port, ssl, upstream_target, scheme);

            logger()->debug("[ws-forward] {} -> {}", req.target(), upstream_url);

            http::fields upstream_headers(req.base());
            upstream_headers.erase(http::field::host);
            upstream_headers.erase(http::field::sec_websocket_key);
            upstream_headers.erase(http::field::sec_websocket_accept);
            upstream_headers.erase(http::field::sec_websocket_version);
            upstream_headers.erase(http::field::upgrade);
            upstream_headers.erase(http::field::connection);
            upstream_headers.set(http::field::host, util::make_host_value(host, port, ssl));

            if (interceptor)
            {
                co_await interceptor->on_upstream_request(req, upstream_headers, upstream_url);
            }

            auto upstream = std::make_shared<client::ws_client>(ex_, host, port, ssl);
            upstream->set_logger(logger());

            auto ec = co_await upstream->async_run(
                upstream_target,
                [upstream, interceptor, conn = websocket_conn::weak_ptr(conn)](std::string_view data,
                                                                               bool binary) -> net::awaitable<void>
                {
                    if (interceptor)
                    {
                        co_await interceptor->on_upstream_recv(data, binary);
                    }
                    if (auto c = conn.lock())
                    {
                        c->send(std::move(data), binary);
                    }
                },
                [upstream, conn = websocket_conn::weak_ptr(conn)]() -> net::awaitable<void>
                {
                    if (auto c = conn.lock())
                    {
                        c->close();
                    }
                    co_return;
                },
                upstream_headers);
            if (ec)
            {
                logger()->trace("[ws-forward] upstream connect failed: {}", ec.message());
                conn->close(ec.message());
                co_return;
            }

            req.set_custom_data(detail::kWsForwardKey, upstream);
            if (interceptor)
            {
                req.set_custom_data(detail::kWsInterceptorKey, interceptor);
            }
        };

        auto message_handler
            = [](websocket_conn::weak_ptr wp, std::string_view data, bool binary) -> net::awaitable<void>
        {
            auto conn = wp.lock();
            if (!conn)
            {
                co_return;
            }

            auto& req = conn->http_request();
            if (!req.has_custom_data(detail::kWsForwardKey))
            {
                conn->close();
                co_return;
            }

            if (req.has_custom_data(detail::kWsInterceptorKey))
            {
                auto wsi = req.custom_data<detail::ws_interceptor_ptr>(detail::kWsInterceptorKey);
                co_await wsi->on_upstream_send(data, binary);
            }

            auto upstream = req.custom_data<detail::ws_client_ptr>(detail::kWsForwardKey);
            auto ec = co_await upstream->async_send(std::string(data), binary);
            if (ec)
            {
                conn->close();
            }
            co_return;
        };

        auto close_handler = [](websocket_conn::weak_ptr wp) -> net::awaitable<void>
        {
            auto conn = wp.lock();
            if (!conn)
            {
                co_return;
            }

            auto& req = conn->http_request();
            if (!req.has_custom_data(detail::kWsForwardKey))
            {
                co_return;
            }
            auto upstream = req.custom_data<detail::ws_client_ptr>(detail::kWsForwardKey);
            if (upstream)
            {
                co_await upstream->async_close();
            }
        };

        router_.set_ws_handler(location, std::move(open_handler), std::move(message_handler), std::move(close_handler));
    }

    void
    http_server::impl::set_proxy_pool_size(size_t max_size)
    {
        proxy_pool_->set_max_size(max_size);
    }

    bool
    http_server::impl::is_open() const
    {
        return !shutting_down_ && acceptor_.is_open();
    }

} // namespace httplib::server
