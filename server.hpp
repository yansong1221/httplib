#pragma once
#include "proxy_conn.hpp"
#include "use_awaitable.hpp"
#include "variant_stream.hpp"
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

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl; // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
namespace websocket = beast::websocket;

// Returns a success response (200)
template<class ResponseBody, class RequestBody>
auto make_response(const beast::http::request<RequestBody> &request,
                   typename ResponseBody::value_type body, beast::string_view content_type,
                   beast::http::status status = beast::http::status::ok) {
    beast::http::response<ResponseBody> response{status, request.version()};
    response.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
    response.set(beast::http::field::content_type, content_type);
    response.body() = body;
    response.prepare_payload();
    response.keep_alive(request.keep_alive());
    return response;
}
template<class RequestBody>
auto make_empty_response(const beast::http::request<RequestBody> &request,
                         beast::http::status status = beast::http::status::ok,
                         std::optional<std::string_view> reason = std::nullopt) {

    beast::http::response<http::empty_body> response{status, request.version()};
    if (reason)
        response.reason(*reason);
    response.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
    response.prepare_payload();
    response.keep_alive(request.keep_alive());
    return response;
}

template<class RequestBody>
auto make_string_response(const beast::http::request<RequestBody> &request, beast::string_view body,
                          beast::string_view content_type,
                          beast::http::status status = beast::http::status::ok) {
    return make_response<http::string_body>(request, body, content_type, status);
}

class server {
private:
    using header_parser_type = http::request_parser<http::empty_body>;

public:
    struct ssl_config {
        std::filesystem::path cert_file;
        std::filesystem::path key_file;
        std::string passwd;
    };

    explicit server(uint32_t num_threads = std::thread::hardware_concurrency())
        : pool_(num_threads), acceptor_(pool_) {
        logger_ = spdlog::stdout_color_mt("server");
        logger_->set_level(spdlog::level::debug);
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
            logger_->debug("accept new connection [{}:{}]",
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
                header_parser_type header_parser;
                while (!header_parser.is_header_done()) {
                    http_variant_stream->expires_after(std::chrono::seconds(30));
                    co_await http::async_read_some(*http_variant_stream, buffer, header_parser,
                                                   net_awaitable[ec]);
                    if (ec) {
                        logger_->debug("read http header failed: {}", ec.message());
                        co_return;
                    }
                }
                http_variant_stream->expires_never();

                const auto &header = header_parser.get();
                // websocket
                if (websocket::is_upgrade(header)) {

                    auto conn = std::make_shared<httplib::websocket_conn>(
                        logger_, std::move(*http_variant_stream));
                    conn->set_open_handler(websocket_open_handler_);
                    conn->set_close_handler(websocket_close_handler_);
                    conn->set_message_handler(websocket_message_handler_);
                    co_await conn->run(header, std::move(buffer));
                    co_return;
                }
                //http proxy
                else if (header.method() == http::verb::connect) {
                    auto target = header.target();
                    auto pos = target.find(":");
                    if (pos == std::string_view::npos)
                        co_return;

                    auto host = target.substr(0, pos);
                    auto port = target.substr(pos + 1);

                    tcp::resolver resolver(co_await net::this_coro::executor);
                    auto results = co_await resolver.async_resolve(host, port, net_awaitable[ec]);
                    if (ec)
                        co_return;

                    tcp::socket proxy_socket(co_await net::this_coro::executor);
                    co_await net::async_connect(proxy_socket, results, net_awaitable[ec]);
                    if (ec)
                        co_return;

                    co_await http::async_write(
                        *http_variant_stream,
                        make_empty_response(header, http::status::ok, "Connection Established"),
                        net_awaitable[ec]);
                    if (ec)
                        co_return;

                    auto conn = std::make_shared<httplib::proxy_conn>(
                        std::move(*http_variant_stream), std::move(proxy_socket));

                    co_await conn->run();
                    //透明代理

                    co_return;
                }
            }
        } catch (const std::exception &e) {
            spdlog::error("do_session: {}", e.what());
        }
        // try {
        //   for (;;) {
        //     header_parser_type header_parser;
        //     while (!header_parser.is_header_done()) {
        //       stream.expires_after(std::chrono::seconds(30));
        //       co_await http::async_read_some(stream, buffer, header_parser);
        //     }
        //     // websocket
        //     if (websocket::is_upgrade(header_parser.get())) {
        //       ws_stream _stream(stream.release_socket());
        //       // wss_stream _wss_stream(stream.release_socket());
        //     }
        //     const auto &header = header_parser.get();
        //     switch (header.method()) {
        //     case http::verb::get: break;
        //     case http::verb::post: {
        //       auto content_type = header[http::field::content_type];
        //       if (content_type.starts_with("application/json")) {}

        //    }

        //    break;
        //    default: break;
        //    }
        //    /*  auto reader = create_body_reader(header.target());
        //      if (!reader) {
        //        reader = std::make_unique<impl_body_reader<http::empty_body>>(
        //            [](const auto &req) -> net::awaitable<http::message_generator>
        //            {
        //              co_return make_string_response(req, "404", "text/plain",
        //                                             http::status::not_found);
        //            });
        //      }*/
        //    http::message_generator msg = make_string_response(
        //        header, "404", "text/plain", http::status::not_found);

        //    // Determine if we should close the connection
        //    bool keep_alive = msg.keep_alive();

        //    // Send the response
        //    co_await beast::async_write(stream, std::move(msg));

        //    if (!keep_alive) {
        //      // This means we should close the connection, usually because
        //      // the response indicated the "Connection: close" semantic.
        //      break;
        //    }
        //  }

        //  // Send a TCP shutdown
        //  stream.socket().shutdown(net::ip::tcp::socket::shutdown_send);

        //  // At this point the connection is closed gracefully
        //  // we ignore the error because the client might have
        //  // dropped the connection already.

        //} catch (const std::exception &e) {}
    }

    template<class Body, class Allocator>
    http::message_generator
    handle_request(beast::string_view doc_root,
                   http::request<Body, http::basic_fields<Allocator>> &&req) {
        // Returns a bad request response
        auto const bad_request = [&req](beast::string_view why) {
            http::response<http::string_body> res{http::status::bad_request, req.version()};
            res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res.set(http::field::content_type, "text/html");
            res.keep_alive(req.keep_alive());
            res.body() = std::string(why);
            res.prepare_payload();
            return res;
        };

        // Returns a not found response
        auto const not_found = [&req](beast::string_view target) {
            http::response<http::string_body> res{http::status::not_found, req.version()};
            res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res.set(http::field::content_type, "text/html");
            res.keep_alive(req.keep_alive());
            res.body() = "The resource '" + std::string(target) + "' was not found.";
            res.prepare_payload();
            return res;
        };

        // Returns a server error response
        auto const server_error = [&req](beast::string_view what) {
            http::response<http::string_body> res{http::status::internal_server_error,
                                                  req.version()};
            res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res.set(http::field::content_type, "text/html");
            res.keep_alive(req.keep_alive());
            res.body() = "An error occurred: '" + std::string(what) + "'";
            res.prepare_payload();
            return res;
        };

        // Make sure we can handle the method
        if (req.method() != http::verb::get && req.method() != http::verb::head)
            return bad_request("Unknown HTTP-method");

        // Request path must be absolute and not contain "..".
        if (req.target().empty() || req.target()[0] != '/' ||
            req.target().find("..") != beast::string_view::npos)
            return bad_request("Illegal request-target");

        // Build the path to the requested file
        std::string path = path_cat(doc_root, req.target());
        if (req.target().back() == '/')
            path.append("index.html");

        // Attempt to open the file
        beast::error_code ec;
        http::file_body::value_type body;
        body.open(path.c_str(), beast::file_mode::scan, ec);

        // Handle the case where the file doesn't exist
        if (ec == beast::errc::no_such_file_or_directory)
            return not_found(req.target());

        // Handle an unknown error
        if (ec)
            return server_error(ec.message());

        // Cache the size since we need it after the move
        auto const size = body.size();

        // Respond to HEAD request
        if (req.method() == http::verb::head) {
            http::response<http::empty_body> res{http::status::ok, req.version()};
            res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res.set(http::field::content_type, mime_type(path));
            res.content_length(size);
            res.keep_alive(req.keep_alive());
            return res;
        }

        // Respond to GET request
        http::response<http::file_body> res{std::piecewise_construct,
                                            std::make_tuple(std::move(body)),
                                            std::make_tuple(http::status::ok, req.version())};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, mime_type(path));
        res.content_length(size);
        res.keep_alive(req.keep_alive());
        return res;
    }
    std::string path_cat(beast::string_view base, beast::string_view path) {
        if (base.empty())
            return std::string(path);
        std::string result(base);
#ifdef BOOST_MSVC
        char constexpr path_separator = '\\';
        if (result.back() == path_separator)
            result.resize(result.size() - 1);
        result.append(path.data(), path.size());
        for (auto &c : result)
            if (c == '/')
                c = path_separator;
#else
        char constexpr path_separator = '/';
        if (result.back() == path_separator)
            result.resize(result.size() - 1);
        result.append(path.data(), path.size());
#endif
        return result;
    }
    beast::string_view mime_type(beast::string_view path) {
        using beast::iequals;
        auto const ext = [&path] {
            auto const pos = path.rfind(".");
            if (pos == beast::string_view::npos)
                return beast::string_view{};
            return path.substr(pos);
        }();
        if (iequals(ext, ".htm"))
            return "text/html";
        if (iequals(ext, ".html"))
            return "text/html";
        if (iequals(ext, ".php"))
            return "text/html";
        if (iequals(ext, ".css"))
            return "text/css";
        if (iequals(ext, ".txt"))
            return "text/plain";
        if (iequals(ext, ".hpp"))
            return "text/plain";
        if (iequals(ext, ".js"))
            return "application/javascript";
        if (iequals(ext, ".json"))
            return "application/json";
        if (iequals(ext, ".xml"))
            return "application/xml";
        if (iequals(ext, ".swf"))
            return "application/x-shockwave-flash";
        if (iequals(ext, ".flv"))
            return "video/x-flv";
        if (iequals(ext, ".png"))
            return "image/png";
        if (iequals(ext, ".jpe"))
            return "image/jpeg";
        if (iequals(ext, ".jpeg"))
            return "image/jpeg";
        if (iequals(ext, ".jpg"))
            return "image/jpeg";
        if (iequals(ext, ".gif"))
            return "image/gif";
        if (iequals(ext, ".bmp"))
            return "image/bmp";
        if (iequals(ext, ".ico"))
            return "image/vnd.microsoft.icon";
        if (iequals(ext, ".tiff"))
            return "image/tiff";
        if (iequals(ext, ".tif"))
            return "image/tiff";
        if (iequals(ext, ".svg"))
            return "image/svg+xml";
        if (iequals(ext, ".svgz"))
            return "image/svg+xml";
        return "application/text";
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

private:
    std::shared_ptr<spdlog::logger> logger_;
    net::thread_pool pool_;
    tcp::acceptor acceptor_;
    std::optional<ssl_config> ssl_config_;

    httplib::websocket_conn::message_handler_type websocket_message_handler_;
    httplib::websocket_conn::open_handler_type websocket_open_handler_;
    httplib::websocket_conn::close_handler_type websocket_close_handler_;
};