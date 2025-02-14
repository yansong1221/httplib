#pragma once
#include "proxy_conn.hpp"
#include "request.hpp"
#include "router.hpp"
#include "use_awaitable.hpp"
#include "variant_stream.hpp"
#include "websocket_conn.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/url.hpp>
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

// 格式化当前时间为 HTTP Date 格式
std::string format_http_date() {

    using namespace std::chrono;

    auto now = utc_clock::now();
    std::time_t tt = system_clock::to_time_t(utc_clock::to_sys(now));
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    return oss.str();
}

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
            logger_->trace("accept new connection [{}:{}]",
                           sock.remote_endpoint().address().to_string(),
                           sock.remote_endpoint().port());

            net::co_spawn(executor, do_session(std::move(sock)), net::detached);
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
    net::awaitable<void> do_session(tcp::socket sock) {
        try {
            std::unique_ptr<util::http_variant_stream_type> http_variant_stream;
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

                util::ssl_http_stream stream(std::move(sock), ssl_ctx);

                auto bytes_used = co_await stream.async_handshake(ssl::stream_base::server,
                                                                  buffer.data(), net_awaitable[ec]);
                if (ec) {
                    logger_->error("ssl handshake failed: {}", ec.message());
                    co_return;
                }
                buffer.consume(bytes_used);
                http_variant_stream =
                    std::make_unique<util::http_variant_stream_type>(std::move(stream));
            } else {
                util::http_stream stream(std::move(sock));
                http_variant_stream =
                    std::make_unique<util::http_variant_stream_type>(std::move(stream));
            }

            for (;;) {
                http::request_parser<http::empty_body> header_parser;
                while (!header_parser.is_header_done()) {
                    http_variant_stream->expires_after(std::chrono::seconds(30));
                    co_await http::async_read_some(*http_variant_stream, buffer, header_parser,
                                                   net_awaitable[ec]);
                    if (ec) {
                        logger_->trace("read http header failed: {}", ec.message());
                        co_return;
                    }
                }
                http_variant_stream->expires_never();

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
                    http::request_parser<http::string_body> body_parser(std::move(header_parser));

                    while (!body_parser.is_done()) {
                        http_variant_stream->expires_after(std::chrono::seconds(30));
                        co_await http::async_read_some(*http_variant_stream, buffer, body_parser,
                                                       net_awaitable[ec]);
                        if (ec) {
                            logger_->trace("read http body failed: {}", ec.message());
                            co_return;
                        }
                    }
                    req = body_parser.release();
                } break;
                }
                httplib::response resp;
                resp.base().result(http::status::not_implemented);
                resp.base().version(req.base().version());
                resp.base().set(http::field::server, BOOST_BEAST_VERSION_STRING);
                resp.base().set(http::field::date, format_http_date());
                resp.keep_alive(req.keep_alive());

                try {
                    co_await handle_http(req, resp);
                } catch (const std::exception &e) {
                    resp.base().result(http::status::internal_server_error);
                    resp.base().set(http::field::content_type, "text/html");
                    resp.set_body<http::string_body>(e.what());
                } catch (...) {
                    resp.base().result(http::status::internal_server_error);
                    resp.base().set(http::field::content_type, "text/html");
                    resp.set_body<http::string_body>("unknown exception");
                }
                resp.base().set(http::field::connection,
                                resp.keep_alive() ? "keep-alive" : "close");
                resp.prepare_payload();
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
    net::awaitable<void> handle_connect(util::http_variant_stream_type http_variant_stream,
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
        resp.base().set(http::field::date, format_http_date());
        co_await http::async_write(http_variant_stream, resp, net_awaitable[ec]);
        if (ec)
            co_return;

        auto conn = std::make_shared<httplib::proxy_conn>(std::move(http_variant_stream),
                                                          std::move(proxy_socket));

        co_await conn->run();
        co_return;
    }
    net::awaitable<void> handle_websocket(util::http_variant_stream_type http_variant_stream,
                                          http::request<http::empty_body> req) {
        auto conn =
            std::make_shared<httplib::websocket_conn>(logger_, std::move(http_variant_stream));
        conn->set_open_handler(websocket_open_handler_);
        conn->set_close_handler(websocket_close_handler_);
        conn->set_message_handler(websocket_message_handler_);
        co_await conn->run(req);
        co_return;
    }
    net::awaitable<void> handle_http(httplib::request &req, httplib::response &resp) {
        
        auto key = std::format("{} {}", req.base().method_string(), req.base().target());
        auto decoded_result = boost::urls::parse_origin_form(key);
        if (!decoded_result.has_value()) {
            logger_->warn("Failed to decode URL: {}", key);
            co_return;
        }
        key = decoded_result->data();

        if (auto handler = router_.get_handler(key); handler) {
            router_.route(handler, req, resp, key);
        } else {
            if (auto coro_handler = router_.get_coro_handler(key); coro_handler) {
                co_await router_.route_coro(coro_handler, req, resp, key);
            } else {
                if (default_handler_) {
                    co_await default_handler_(req, resp);
                } else {
                    bool is_exist = false;

                    std::function<void(request & req, response & resp)> handler;
                    std::string method_str{req.base().method_string()};
                    std::string url_path = method_str;
                    url_path.append(" ").append(req.base().target());
                    std::tie(is_exist, handler, req.params) =
                        router_.get_router_tree()->get(url_path, method_str);
                    if (is_exist) {
                        if (handler) {
                            (handler)(req, resp);
                        } else {
                            resp.base().result(http::status::not_found);
                        }
                    } else {
                        bool is_coro_exist = false;
                        std::function<net::awaitable<void>(request & req, response & resp)>
                            coro_handler;

                        std::tie(is_coro_exist, coro_handler, req.params) =
                            router_.get_coro_router_tree()->get_coro(url_path, method_str);

                        if (is_coro_exist) {
                            if (coro_handler) {
                                co_await coro_handler(req, resp);
                            } else {
                                resp.base().result(http::status::not_found);
                            }
                        } else {
                            bool is_matched_regex_router = false;
                            // coro regex router
                            auto coro_regex_handlers = router_.get_coro_regex_handlers();
                            if (coro_regex_handlers.size() != 0) {
                                for (auto &pair : coro_regex_handlers) {
                                    std::string coro_regex_key{key};

                                    if (std::regex_match(coro_regex_key, req.matches,
                                                         std::get<0>(pair))) {
                                        auto coro_handler = std::get<1>(pair);
                                        if (coro_handler) {
                                            co_await coro_handler(req, resp);
                                            is_matched_regex_router = true;
                                        }
                                    }
                                }
                            }
                            // regex router
                            if (!is_matched_regex_router) {
                                auto regex_handlers = router_.get_regex_handlers();
                                if (regex_handlers.size() != 0) {
                                    for (auto &pair : regex_handlers) {
                                        std::string regex_key{key};
                                        if (std::regex_match(regex_key, req.matches,
                                                             std::get<0>(pair))) {
                                            auto handler = std::get<1>(pair);
                                            if (handler) {
                                                (handler)(req, resp);
                                                is_matched_regex_router = true;
                                            }
                                        }
                                    }
                                }
                            }
                            // not found
                            if (!is_matched_regex_router) {
                                resp.base().result(http::status::not_found);
                            }
                        }
                    }
                }
            }
        }

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

private:
    std::shared_ptr<spdlog::logger> logger_;
    httplib::router router_;
    net::thread_pool pool_;
    tcp::acceptor acceptor_;
    std::optional<ssl_config> ssl_config_;

    httplib::websocket_conn::message_handler_type websocket_message_handler_;
    httplib::websocket_conn::open_handler_type websocket_open_handler_;
    httplib::websocket_conn::close_handler_type websocket_close_handler_;

    std::function<net::awaitable<void>(request &, response &)> default_handler_;
};
} // namespace httplib