#pragma once
#include "use_awaitable.hpp"
#include "variant_stream.hpp"
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
                   typename ResponseBody::value_type body,
                   beast::string_view content_type,
                   beast::http::status status = beast::http::status::ok)
{
    beast::http::response<ResponseBody> response{status, request.version()};
    response.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
    response.set(beast::http::field::content_type, content_type);
    response.body() = body;
    response.prepare_payload();
    response.keep_alive(request.keep_alive());
    return response;
}
template<class RequestBody>
auto make_string_response(const beast::http::request<RequestBody> &request,
                          beast::string_view body,
                          beast::string_view content_type,
                          beast::http::status status = beast::http::status::ok)
{
    return make_response<http::string_body>(request, body, content_type, status);
}

using http_stream = beast::tcp_stream;
using ssl_http_stream = beast::ssl_stream<beast::tcp_stream>;
using http_variant_stream_type = util::http_variant_stream<http_stream, ssl_http_stream>;

using ws_stream = websocket::stream<http_stream>;
using ssl_ws_stream = websocket::stream<ssl_http_stream>;
using ws_variant_stream_type = util::websocket_variant_stream<ws_stream, ssl_ws_stream>;

class connection_ssl
{
public:
    connection_ssl(std::unique_ptr<ssl::context> &&ssl_ctx)
        : ssl_ctx_(std::move(ssl_ctx))
    {}

private:
    std::unique_ptr<ssl::context> ssl_ctx_;
};

class websocket_conn : public std::enable_shared_from_this<websocket_conn>
{
private:
    using binary_data_type = std::vector<uint8_t>;
    using text_data_type = std::string;
    using write_data_type = std::variant<binary_data_type, text_data_type>;

public:
    websocket_conn(std::shared_ptr<spdlog::logger> logger, http_variant_stream_type &&stream)
        : logger_(logger)
        , strand_(stream.get_executor())
    {
        std::visit(
            [this](auto &&t) {
                using value_type = std::decay_t<decltype(t)>;
                if constexpr (std::same_as<http_stream, value_type>) {
                    ws_ = std::make_unique<ws_variant_stream_type>(ws_stream(std::move(t)));
                } else if constexpr (std::same_as<ssl_http_stream, value_type>) {
                    ws_ = std::make_unique<ws_variant_stream_type>(ssl_ws_stream(std::move(t)));
                } else {
                    static_assert(false, "unknown http_variant_stream_type");
                }
            },
            stream);
    }
    void send_data(std::span<uint8_t> data)
    {
        binary_data_type binary_data(data.begin(), data.end());
        send_data(std::move(binary_data));
    }
    void send_data(write_data_type &&data)
    {
        if (!ws_->is_open())
            return;

        net::post(strand_, [this, data = std::move(data), self = shared_from_this()]() {
            bool send_in_process = !send_que_.empty();
            send_que_.emplace_back(std::move(data));
            if (send_in_process)
                return;
            net::co_spawn(strand_, process_write_data(), net::detached);
        });
    }
    void close()
    {
        if (!ws_->is_open())
            return;

        net::co_spawn(
            strand_,
            [this, self = shared_from_this()]() -> net::awaitable<void> {
                boost::system::error_code ec;
                co_await net::post(strand_, net_awaitable[ec]);
                if (ec)
                    co_return;

                websocket::close_reason reason("normal");
                co_await ws_->async_close(reason, net_awaitable[ec]);
            },
            net::detached);
    }

public:
    net::awaitable<void> process_write_data()
    {
        auto self = shared_from_this();

        for (;;) {
            boost::system::error_code ec;
            co_await net::post(strand_, net_awaitable[ec]);
            if (ec)
                co_return;
            if (send_que_.empty())
                co_return;

            write_data_type data = std::move(send_que_.front());
            send_que_.pop_front();

            co_await std::visit(
                [&](auto &v) -> net::awaitable<void> {
                    using value_type = std::decay_t<decltype(v)>;
                    if constexpr (std::same_as<value_type, text_data_type>) {
                        ws_->text(true);
                    } else {
                        ws_->binary(true);
                    }
                    co_await ws_->async_write(net::buffer(v), net_awaitable[ec]);
                },
                data);
            if (ec)
                co_return;
        }
    }
    net::awaitable<void> run(http::request<http::empty_body> const &req, beast::flat_buffer buffer)
    {
        boost::system::error_code ec;
        co_await ws_->async_accept(req, net_awaitable[ec]);
        if (ec) {
            logger_->error("websocket handshake failed: {}", ec.message());
            co_return;
        }
        auto remote_endp = ws_->remote_endpoint();

        logger_->debug("websocket new connection: [{}:{}]",
                       remote_endp.address().to_string(),
                       remote_endp.port());

        for (;;) {
            auto bytes = co_await ws_->async_read(buffer, net_awaitable[ec]);
            if (ec) {
                logger_->debug("websocket disconnect: [{}:{}] what: {}",
                               remote_endp.address().to_string(),
                               remote_endp.port(),
                               ec.message());
                co_return;
            }

            if (ws_->got_text()) {
                std::string_view data(net::buffer_cast<const char *>(buffer.data()),
                                      net::buffer_size(buffer.data()));
                //for (int i = 0; i < 1000; ++i)
                send_data(std::string(data));
                //co_await ws_->async_write(net::buffer(data), net_awaitable[ec]);
                close();

                //send_data(std::string(data));
            }

            buffer.consume(bytes);
        }
        //ws_stream w;
        //w.async_write
        //    w.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        //w.async_read
    }

private:
    net::strand<net::any_io_executor> strand_;
    std::shared_ptr<spdlog::logger> logger_;
    std::unique_ptr<ws_variant_stream_type> ws_;

    std::list<write_data_type> send_que_;
};

class server
{
private:
    using header_parser_type = http::request_parser<http::empty_body>;

public:
    struct ssl_config
    {
        std::filesystem::path cert_file;
        std::filesystem::path key_file;
        std::string passwd;
    };

    explicit server(uint32_t num_threads = std::thread::hardware_concurrency())
        : pool_(num_threads)
        , acceptor_(pool_)
    {
        logger_ = spdlog::stdout_color_mt("server");
        logger_->set_level(spdlog::level::debug);
        ssl_config_ = ssl_config{R"(D:\code\http\server.crt)", R"(D:\code\http\server.key)", "test"};
        //ssl_config_ = ssl_config{};
    }

public:
    auto get_executor() noexcept { return pool_.get_executor(); }

    server &listen(std::string_view host,
                   uint16_t port,
                   int backlog = net::socket_base::max_listen_connections)
    {
        tcp::resolver resolver(pool_);
        auto results = resolver.resolve(host, std::to_string(port));

        tcp::endpoint endp(*results.begin());
        acceptor_.open(endp.protocol());
        acceptor_.bind(endp);
        acceptor_.listen(backlog);
        logger_->info("Server Listen on: [{}:{}]", endp.address().to_string(), endp.port());
        return *this;
    }
    void run()
    {
        async_run();
        pool_.wait();
    }

    void async_run() { net::co_spawn(pool_, do_listen(), net::detached); }

public:
    net::awaitable<void> do_listen()
    {
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
    std::unique_ptr<ssl::context> create_ssl_context()
    {
        try {
            unsigned long ssl_options = ssl::context::default_workarounds | ssl::context::no_sslv2
                                        | ssl::context::single_dh_use;

            auto ssl_ctx = std::make_unique<ssl::context>(ssl::context::sslv23);
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
    net::awaitable<void> do_session(tcp::socket sock)
    {
        try {
            beast::flat_buffer buffer;
            boost::system::error_code ec;
            bool is_ssl = co_await beast::async_detect_ssl(sock, buffer, net_awaitable[ec]);

            if (ec)
                co_return;

            std::unique_ptr<http_variant_stream_type> http_variant_stream;
            std::unique_ptr<ssl::context> ssl_ctx;
            if (is_ssl) {
                ssl_ctx = create_ssl_context();
                if (!ssl_ctx)
                    co_return;

                ssl_http_stream stream(std::move(sock), *ssl_ctx);

                auto bytes_used = co_await stream.async_handshake(ssl::stream_base::server,
                                                                  buffer.data(),
                                                                  net_awaitable[ec]);
                if (ec) {
                    logger_->error("ssl handshake failed: {}", ec.message());
                    co_return;
                }
                buffer.consume(bytes_used);
                http_variant_stream = std::make_unique<http_variant_stream_type>(std::move(stream));

            } else {
                http_stream stream(std::move(sock));
                http_variant_stream = std::make_unique<http_variant_stream_type>(std::move(stream));
            }

            for (;;) {
                header_parser_type header_parser;
                while (!header_parser.is_header_done()) {
                    http_variant_stream->expires_after(std::chrono::seconds(30));
                    co_await http::async_read_some(*http_variant_stream, buffer, header_parser);
                }
                // websocket
                if (websocket::is_upgrade(header_parser.get())) {
                    http_variant_stream->expires_never();
                    auto conn = std::make_shared<websocket_conn>(logger_,
                                                                 std::move(*http_variant_stream));
                    co_await conn->run(header_parser.get(), std::move(buffer));
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
    http::message_generator handle_request(beast::string_view doc_root,
                                           http::request<Body, http::basic_fields<Allocator>> &&req)
    {
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
        if (req.target().empty() || req.target()[0] != '/'
            || req.target().find("..") != beast::string_view::npos)
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
    std::string path_cat(beast::string_view base, beast::string_view path)
    {
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
    beast::string_view mime_type(beast::string_view path)
    {
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

private:
    std::shared_ptr<spdlog::logger> logger_;
    net::thread_pool pool_;
    tcp::acceptor acceptor_;
    std::optional<ssl_config> ssl_config_;
};