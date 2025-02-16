#pragma once
#include "html.hpp"
#include "proxy_conn.hpp"
#include "request.hpp"
#include "router.hpp"
#include "stream/variant_stream.hpp"
#include "use_awaitable.hpp"
#include "websocket_conn.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <span>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
namespace httplib {
namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl; // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
namespace websocket = beast::websocket;

class server {

public:
    struct ssl_config {
        std::filesystem::path cert_file;
        std::filesystem::path key_file;
        std::string passwd;
    };

    explicit server(uint32_t num_threads = std::thread::hardware_concurrency())
        : pool_(num_threads),
          acceptor_(pool_),
          logger_(spdlog::stdout_color_mt("server")),
          router_(logger_) {

        logger_->set_level(spdlog::level::trace);
        ssl_config_ =
            ssl_config{R"(D:\code\http\server.crt)", R"(D:\code\http\server.key)", "test"};
        // ssl_config_ = ssl_config{};
    }

public:
    auto get_executor() noexcept {
        return pool_.get_executor();
    }

    server &listen(std::string_view host, uint16_t port,
                   int backlog = net::socket_base::max_listen_connections) {
        tcp::resolver resolver(pool_);
        auto results = resolver.resolve(host, std::to_string(port));

        tcp::endpoint endp(*results.begin());
        acceptor_.open(endp.protocol());
        acceptor_.bind(endp);
        acceptor_.listen(backlog);
        logger_->info("Server Listen on: [{}:{}]", endp.address().to_string(), endp.port());
        return *this;
    }
    void run() {
        async_run();
        pool_.wait();
    }

    void async_run() {
        net::co_spawn(pool_, do_listen(), net::detached);
    }

public:
    net::awaitable<void> do_listen() {
        boost::system::error_code ec;

        const auto &executor = co_await net::this_coro::executor;
        for (;;) {
            tcp::socket sock(executor);
            co_await acceptor_.async_accept(sock, net_awaitable[ec]);
            if (ec)
                co_return;

            net::co_spawn(
                executor,
                [this, sock = std::move(sock)]() mutable -> net::awaitable<void> {
                    auto remote_endp = sock.remote_endpoint();
                    logger_->trace("accept new connection [{}:{}]",
                                   remote_endp.address().to_string(), remote_endp.port());
                    co_await do_session(std::move(sock));
                    logger_->trace("close connection [{}:{}]", remote_endp.address().to_string(),
                                   remote_endp.port());
                },
                net::detached);
        }
    }

private:
    std::shared_ptr<ssl::context> create_ssl_context() {
        try {
            unsigned long ssl_options = ssl::context::default_workarounds | ssl::context::no_sslv2 |
                                        ssl::context::single_dh_use;

            auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
            ssl_ctx->set_options(ssl_options);

            if (!ssl_config_->passwd.empty()) {
                ssl_ctx->set_password_callback([this](auto, auto) { return ssl_config_->passwd; });
            }
            ssl_ctx->use_certificate_chain_file(ssl_config_->cert_file.string());
            ssl_ctx->use_private_key_file(ssl_config_->key_file.string(), ssl::context::pem);
            return ssl_ctx;
        } catch (const std::exception &e) {
            logger_->error("create_ssl_context: {}", e.what());
            return nullptr;
        }
    }

    template<class Body>
    net::awaitable<void>
    async_read_http_body(http::request_parser<http::empty_body> &&header_parser,
                         stream::http_variant_stream_type &stream, beast::flat_buffer &buffer,
                         httplib::request &req, boost::system::error_code &ec) {

        http::request_parser<Body> body_parser(std::move(header_parser));

        while (!body_parser.is_done()) {
            stream.expires_after(std::chrono::seconds(30));
            co_await http::async_read_some(stream, buffer, body_parser, net_awaitable[ec]);
            stream.expires_never();
            if (ec) {
                logger_->trace("read http body failed: {}", ec.message());
                co_return;
            }
        }
        req = std::move(body_parser.release());
        co_return;
    }

    net::awaitable<void> do_session(tcp::socket sock) {
        try {
            net::ip::tcp::endpoint remote_endpoint = sock.remote_endpoint();
            net::ip::tcp::endpoint local_endpoint = sock.local_endpoint();

            std::unique_ptr<stream::http_variant_stream_type> http_variant_stream;
            beast::flat_buffer buffer;
            boost::system::error_code ec;
            bool is_ssl = co_await beast::async_detect_ssl(sock, buffer, net_awaitable[ec]);
            if (ec) {
                logger_->error("async_detect_ssl failed: {}", ec.message());
                co_return;
            }
            if (is_ssl) {
                auto ssl_ctx = create_ssl_context();
                if (!ssl_ctx)
                    co_return;

                stream::ssl_http_stream stream(std::move(sock), ssl_ctx);
                auto bytes_used = co_await stream.async_handshake(ssl::stream_base::server,
                                                                  buffer.data(), net_awaitable[ec]);
                if (ec) {
                    logger_->error("ssl handshake failed: {}", ec.message());
                    co_return;
                }
                buffer.consume(bytes_used);
                http_variant_stream =
                    std::make_unique<stream::http_variant_stream_type>(std::move(stream));
            } else {
                stream::http_stream stream(std::move(sock));
                http_variant_stream =
                    std::make_unique<stream::http_variant_stream_type>(std::move(stream));
            }

            for (;;) {
                http::request_parser<http::empty_body> header_parser;
                while (!header_parser.is_header_done()) {
                    http_variant_stream->expires_after(std::chrono::seconds(30));
                    co_await http::async_read_some(*http_variant_stream, buffer, header_parser,
                                                   net_awaitable[ec]);
                    http_variant_stream->expires_never();
                    if (ec) {
                        logger_->trace("read http header failed: {}", ec.message());
                        co_return;
                    }
                }

                const auto &header = header_parser.get();

                // websocket
                if (websocket::is_upgrade(header)) {
                    co_await handle_websocket(std::move(*http_variant_stream),
                                              header_parser.release());
                    co_return;
                }
                //http proxy
                else if (header.method() == http::verb::connect) {
                    co_await handle_connect(std::move(*http_variant_stream),
                                            header_parser.release());
                    co_return;
                }

                httplib::request req;
                switch (header.base().method()) {
                case http::verb::get:
                case http::verb::head:
                case http::verb::trace:
                case http::verb::connect:
                    req = header_parser.release();
                    break;
                default: {

                    auto content_type = header[http::field::content_type];
                    if (content_type.starts_with("multipart/form-data")) {

                        co_await async_read_http_body<form_data_body>(
                            std::move(header_parser), *http_variant_stream, buffer, req, ec);
                    } else {
                        co_await async_read_http_body<http::string_body>(
                            std::move(header_parser), *http_variant_stream, buffer, req, ec);
                    }
                    if (ec)
                        co_return;
                } break;
                }
                httplib::response resp;
                co_await router_.routing(req, resp);

                auto msg = resp.to_message_generator();

                // Determine if we should close the connection
                bool keep_alive = msg.keep_alive();

                co_await beast::async_write(*http_variant_stream, std::move(msg),
                                            net_awaitable[ec]);

                if (!keep_alive) {
                    // This means we should close the connection, usually because
                    // the response indicated the "Connection: close" semantic.
                    boost::system::error_code ec;
                    http_variant_stream->shutdown(net::socket_base::shutdown_receive, ec);
                    co_return;
                }
            }
        } catch (const std::exception &e) {
            spdlog::error("do_session: {}", e.what());
        }
    }
    net::awaitable<void> handle_connect(stream::http_variant_stream_type http_variant_stream,
                                        http::request<http::empty_body> req) {
        auto target = req.target();
        auto pos = target.find(":");
        if (pos == std::string_view::npos)
            co_return;

        auto host = target.substr(0, pos);
        auto port = target.substr(pos + 1);

        boost::system::error_code ec;
        tcp::resolver resolver(co_await net::this_coro::executor);
        auto results = co_await resolver.async_resolve(host, port, net_awaitable[ec]);
        if (ec)
            co_return;

        tcp::socket proxy_socket(co_await net::this_coro::executor);
        co_await net::async_connect(proxy_socket, results, net_awaitable[ec]);
        if (ec)
            co_return;

        http::response<http::empty_body> resp(http::status::ok, req.version());
        resp.base().reason("Connection Established");
        resp.base().set(http::field::server, BOOST_BEAST_VERSION_STRING);
        resp.base().set(http::field::date, html::format_http_date());
        co_await http::async_write(http_variant_stream, resp, net_awaitable[ec]);
        if (ec)
            co_return;

        auto conn = std::make_shared<httplib::proxy_conn>(std::move(http_variant_stream),
                                                          std::move(proxy_socket));

        co_await conn->run();
        co_return;
    }
    net::awaitable<void> handle_websocket(stream::http_variant_stream_type http_variant_stream,
                                          http::request<http::empty_body> req) {
        auto conn =
            std::make_shared<httplib::websocket_conn>(logger_, std::move(http_variant_stream));
        conn->set_open_handler(websocket_open_handler_);
        conn->set_close_handler(websocket_close_handler_);
        conn->set_message_handler(websocket_message_handler_);
        co_await conn->run(req);
        co_return;
    }

public:
    void set_websocket_message_handler(httplib::websocket_conn::message_handler_type &&handler) {
        websocket_message_handler_ = handler;
    }
    void set_websocket_open_handler(httplib::websocket_conn::open_handler_type &&handler) {
        websocket_open_handler_ = handler;
    }
    void set_websocket_close_handler(httplib::websocket_conn::close_handler_type &&handler) {
        websocket_close_handler_ = handler;
    }

    template<http::verb... method, typename Func, typename... Aspects>
    void set_http_handler(std::string key, Func handler, Aspects &&...asps) {
        static_assert(sizeof...(method) >= 1, "must set http_method");
        if constexpr (sizeof...(method) == 1) {
            (router_.set_http_handler<method>(std::move(key), std::move(handler),
                                              std::forward<Aspects>(asps)...),
             ...);
        } else {
            (router_.set_http_handler<method>(key, handler, std::forward<Aspects>(asps)...), ...);
        }
    }

    template<http::verb... method, typename Func, typename... Aspects>
    void set_http_handler(std::string key, Func handler, util::class_type_t<Func> &owner,
                          Aspects &&...asps) {
        static_assert(std::is_member_function_pointer_v<Func>, "must be member function");
        using return_type = typename util::function_traits<Func>::return_type;
        if constexpr (is_awaitable_v<return_type>) {
            std::function<net::awaitable<void>(httplib::request & req, httplib::response & resp)>
                f = std::bind(handler, &owner, std::placeholders::_1, std::placeholders::_2);
            set_http_handler<method...>(std::move(key), std::move(f),
                                        std::forward<Aspects>(asps)...);
        } else {
            std::function<void(httplib::request & req, httplib::response & resp)> f =
                std::bind(handler, &owner, std::placeholders::_1, std::placeholders::_2);
            set_http_handler<method...>(std::move(key), std::move(f),
                                        std::forward<Aspects>(asps)...);
        }
    }

    bool set_mount_point(const std::string &mount_point, const std::filesystem::path &dir,
                         const http::fields &headers = {}) {

        return router_.set_mount_point(mount_point, dir, headers);
    }
    bool remove_mount_point(const std::string &mount_point) {
        return router_.remove_mount_point(mount_point);
    }

private:
    std::shared_ptr<spdlog::logger> logger_;
    httplib::router router_;
    net::thread_pool pool_;
    tcp::acceptor acceptor_;
    std::optional<ssl_config> ssl_config_;

    httplib::websocket_conn::message_handler_type websocket_message_handler_;
    httplib::websocket_conn::open_handler_type websocket_open_handler_;
    httplib::websocket_conn::close_handler_type websocket_close_handler_;
};
} // namespace httplib