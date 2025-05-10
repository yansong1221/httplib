#include "session.hpp"

#include "body/compressor.hpp"
#include "httplib/response.hpp"
#include "httplib/router.hpp"
#include "httplib/server.hpp"
#include "httplib/setting.hpp"
#include "websocket_conn_impl.hpp"
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/detect_ssl.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/websocket/rfc6455.hpp>

namespace httplib {

namespace detail {

template<class Body>
httplib::response make_respone(const http::request<Body>& req)
{
    httplib::response resp;
    resp.result(http::status::not_found);
    resp.version(req.version());
    resp.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    resp.set(http::field::date, html::format_http_current_gmt_date());
    resp.keep_alive(req.keep_alive());
    return resp;
}
#ifdef HTTPLIB_ENABLED_SSL
static std::shared_ptr<ssl::context> create_ssl_context(const server::SSLConfig& ssl_conf,
                                                        boost::system::error_code& ec)
{
    unsigned long ssl_options =
        ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::single_dh_use;

    auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
    ssl_ctx->set_options(ssl_options, ec);
    if (ec)
        return nullptr;

    if (!ssl_conf.passwd.empty()) {
        ssl_ctx->set_password_callback([pass = ssl_conf.passwd](auto, auto) { return pass; }, ec);
        if (ec)
            return nullptr;
    }
    ssl_ctx->use_certificate_chain_file(ssl_conf.cert_file.string(), ec);
    if (ec)
        return nullptr;

    ssl_ctx->use_private_key_file(ssl_conf.key_file.string(), ssl::context::pem, ec);
    if (ec)
        return nullptr;

    return ssl_ctx;
}
#endif

template<typename S1, typename S2>
net::awaitable<void> transfer(S1& from, S2& to, size_t& bytes_transferred)
{
    static constexpr int buffer_size = 512 * 1024;

    bytes_transferred = 0;
    std::vector<uint8_t> buffer(buffer_size);
    boost::system::error_code ec;

    for (;;) {
        auto bytes = co_await from.async_read_some(net::buffer(buffer), net_awaitable[ec]);
        if (ec) {
            if (bytes > 0)
                co_await net::async_write(to, net::buffer(buffer, bytes), net_awaitable[ec]);

            to.shutdown(net::socket_base::shutdown_send, ec);
            co_return;
        }
        co_await net::async_write(to, net::buffer(buffer, bytes), net_awaitable[ec]);
        if (ec) {
            to.shutdown(net::socket_base::shutdown_send, ec);
            from.shutdown(net::socket_base::shutdown_receive, ec);
            co_return;
        }
        bytes_transferred += bytes;
    }
}


class websocket_task : public session::task
{
public:
    explicit websocket_task(websocket_variant_stream_type&& stream,
                            request&& req,
                            const server::setting& option)
        : req_(std::move(req))
    {
        auto conn = std::make_shared<httplib::websocket_conn_impl>(option, std::move(stream));
    }
    net::awaitable<std::unique_ptr<task>> then() override
    {
        co_await conn_->run(req_);
        co_return nullptr;
    }

    void abort() override { conn_->close(); }

private:
    std::shared_ptr<httplib::websocket_conn_impl> conn_;
    request req_;
};

class http_proxy_task : public session::task
{
public:
    explicit http_proxy_task(http_variant_stream_type&& stream,
                             request&& req,
                             const server::setting& option)
        : stream_(std::move(stream))
        , req_(std::move(req))
        , option_(option)
        , resolver_(stream_.get_executor())
        , proxy_socket_(stream_.get_executor())
    {
    }

public:
    net::awaitable<std::unique_ptr<task>> then() override
    {
        auto target = req_.target();
        auto pos    = target.find(":");
        if (pos == std::string_view::npos)
            co_return nullptr;

        auto host = target.substr(0, pos);
        auto port = target.substr(pos + 1);

        boost::system::error_code ec;
        auto results = co_await resolver_.async_resolve(host, port, net_awaitable[ec]);
        if (ec)
            co_return nullptr;

        co_await net::async_connect(proxy_socket_, results, net_awaitable[ec]);
        if (ec)
            co_return nullptr;

        auto resp = detail::make_respone(req_);
        resp.reason("Connection Established");
        resp.result(http::status::ok);
        co_await http::async_write(stream_, resp, net_awaitable[ec]);
        if (ec)
            co_return nullptr;

        // proxy
        using namespace net::experimental::awaitable_operators;
        size_t l2r_transferred = 0;
        size_t r2l_transferred = 0;
        co_await (detail::transfer(stream_, proxy_socket_, l2r_transferred) &&
                  detail::transfer(proxy_socket_, stream_, r2l_transferred));
        co_return nullptr;
    }

    void abort() override
    {
        boost::system::error_code ec;
        stream_.close(ec);
        resolver_.cancel();
        proxy_socket_.close(ec);
    }

private:
    http_variant_stream_type stream_;
    tcp::resolver resolver_;
    tcp::socket proxy_socket_;

    request req_;
    const server::setting& option_;
};


class http_task : public session::task
{
public:
    explicit http_task(http_variant_stream_type&& stream,
                       beast::flat_buffer&& buffer,
                       httplib::router& router,
                       const server::setting& option)
        : option_(option)
        , router_(router)
        , buffer_(std::move(buffer))
        , stream_(std::move(stream))
    {
        local_endpoint_  = stream_.local_endpoint();
        remote_endpoint_ = stream_.remote_endpoint();
    }


    net::awaitable<std::unique_ptr<task>> then() override
    {
        for (;;) {
            boost::system::error_code ec;
            http::request_parser<http::empty_body> header_parser;
            header_parser.body_limit(std::numeric_limits<unsigned long long>::max());
            while (!header_parser.is_header_done()) {
                stream_.expires_after(option_.read_timeout);
                co_await http::async_read_some(stream_, buffer_, header_parser, net_awaitable[ec]);
                stream_.expires_never();
                if (ec) {
                    option_.get_logger()->trace("read http header failed: {}", ec.message());
                    co_return nullptr;
                }
            }

            const auto& header = header_parser.get();

            // websocket
            if (websocket::is_upgrade(header)) {
#ifdef HTTPLIB_ENABLED_WEBSOCKET
                auto stream = create_websocket_variant_stream(std::move(stream_));
                request req(header_parser.release());
                co_return std::make_unique<websocket_task>(
                    std::move(stream), std::move(req), option_);
#endif // HTTPLIB_ENABLED_WEBSOCKET
                co_return nullptr;
            }
            // http proxy
            else if (header.method() == http::verb::connect) {
                request req(header_parser.release());
                co_return std::make_unique<http_proxy_task>(
                    std::move(stream_), std::move(req), option_);
            }
            httplib::response resp = detail::make_respone(header);
            httplib::request req;
            if (router_.has_handler(header.method(), header.target())) {
                switch (header.method()) {
                    case http::verb::get:
                    case http::verb::head:
                    case http::verb::trace:
                    case http::verb::connect:
                        req = httplib::request(header_parser.release());
                        break;
                    default: {
                        http::request_parser<body::any_body> body_parser(std::move(header_parser));
                        while (!body_parser.is_done()) {
                            stream_.expires_after(option_.read_timeout);
                            co_await http::async_read_some(
                                stream_, buffer_, body_parser, net_awaitable[ec]);
                            stream_.expires_never();
                            if (ec) {
                                option_.get_logger()->trace("read http body failed: {}",
                                                            ec.message());
                                co_return nullptr;
                            }
                        }
                        req = body_parser.release();
                    } break;
                }
                // init request
                req.local_endpoint  = local_endpoint_;
                req.remote_endpoint = remote_endpoint_;

                auto start_time = std::chrono::steady_clock::now();

                co_await router_.routing(req, resp);

                auto span_time = std::chrono::steady_clock::now() - start_time;

                option_.get_logger()->info(
                    "{} {} ({}:{} -> {}:{}) {} {}ms",
                    req.method_string(),
                    req.target(),
                    remote_endpoint_.address().to_string(),
                    remote_endpoint_.port(),
                    local_endpoint_.address().to_string(),
                    local_endpoint_.port(),
                    resp.result_int(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(span_time).count());
            }

            for (const auto& encoding : util::split(req[http::field::accept_encoding], ",")) {
                if (body::compressor_factory::instance().is_supported_encoding(encoding)) {
                    resp.set(http::field::content_encoding, encoding);
                    resp.chunked(true);
                    break;
                }
            }

            if (!resp.has_content_length())
                resp.prepare_payload();

            http::response_serializer<body::any_body> serializer(resp);
            while (!serializer.is_done()) {
                stream_.expires_after(option_.write_timeout);
                co_await http::async_write_some(stream_, serializer, net_awaitable[ec]);
                stream_.expires_never();
                if (ec) {
                    option_.get_logger()->trace("write http body failed: {}", ec.message());
                    co_return nullptr;
                }
            }

            if (!resp.keep_alive()) {
                // This means we should close the connection, usually
                // because the response indicated the "Connection: close"
                // semantic.
                boost::system::error_code ec;
                stream_.shutdown(net::socket_base::shutdown_send, ec);
                co_return nullptr;
            }
        }
        co_return nullptr;
    }


    void abort() override
    {
        boost::system::error_code ec;
        stream_.close(ec);
    }

private:
    const server::setting& option_;
    httplib::router& router_;
    http_variant_stream_type stream_;
    beast::flat_buffer buffer_;

    tcp::endpoint local_endpoint_;
    tcp::endpoint remote_endpoint_;
};
#ifdef HTTPLIB_ENABLED_SSL
class ssl_handshake_task : public session::task
{
public:
    explicit ssl_handshake_task(ssl_http_stream&& stream,
                                beast::flat_buffer&& buffer,
                                httplib::router& router,
                                const server::Option& option)
        : option_(option)
        , router_(router)
        , stream_(std::move(stream))
        , buffer_(std::move(buffer))
    {
    }


    net::awaitable<std::unique_ptr<task>> then() override
    {
        boost::system::error_code ec;
        auto bytes_used = co_await stream_.async_handshake(
            ssl::stream_base::server, buffer_.data(), net_awaitable[ec]);
        if (ec) {
            option_.get_logger()->error("ssl handshake failed: {}", ec.message());
            co_return nullptr;
        }
        buffer_.consume(bytes_used);

        http_variant_stream_type variant_stream(std::move(stream_));
        co_return std::make_unique<http_task>(
            std::move(variant_stream), std::move(buffer_), router_, option_);
    }


    void abort() override { stream_.shutdown(); }

private:
    const server::setting& option_;
    httplib::router& router_;
    ssl_http_stream stream_;
    beast::flat_buffer buffer_;
};
#endif

class detect_ssl_task : public session::task
{
public:
    explicit detect_ssl_task(tcp::socket&& stream,
                             const server::setting& option,
                             httplib::router& router)
        : option_(option)
        , router_(router)
        , stream_(std::move(stream))
    {
        stream_.expires_after(option_.read_timeout);
    }
    ~detect_ssl_task() { stream_.expires_never(); }

public:
    net::awaitable<std::unique_ptr<task>> then() override
    {
        beast::flat_buffer buffer;
#ifdef HTTPLIB_ENABLED_SSL
        if (option_.ssl_conf) {
            boost::system::error_code ec;
            bool is_ssl = co_await beast::async_detect_ssl(stream_, buffer, net_awaitable[ec]);
            if (ec) {
                option_.get_logger()->error("async_detect_ssl failed: {}", ec.message());
                co_return nullptr;
            }
            if (is_ssl) {
                auto ssl_ctx = detail::create_ssl_context(*option_.ssl_conf, ec);
                if (!ssl_ctx) {
                    option_.get_logger()->error("create_ssl_context failed: {}", ec.message());
                    co_return nullptr;
                }
                ssl_http_stream use_ssl_stream(std::move(stream_), ssl_ctx);
                co_return std::make_unique<ssl_handshake_task>(
                    std::move(use_ssl_stream), std::move(buffer), router_, option_);
            }
        }
#endif
        http_variant_stream_type variant_stream(std::move(stream_));
        co_return std::make_unique<http_task>(
            std::move(variant_stream), std::move(buffer), router_, option_);
    }

    void abort() override { stream_.close(); }

private:
    const server::setting& option_;
    httplib::router& router_;
    http_stream stream_;
};

} // namespace detail

session::session(tcp::socket&& stream, const server::setting& option, httplib::router& router)
    : option_(option)
{
    remote_endpoint_ = stream.remote_endpoint();
    local_endpoint_  = stream.local_endpoint();
    option_.get_logger()->trace("accept new connection [{}:{}]",
                                remote_endpoint_.address().to_string(),
                                remote_endpoint_.port());
    task_ = std::make_unique<detail::detect_ssl_task>(std::move(stream), option, router);
}

session::~session()
{
    option_.get_logger()->trace("close connection [{}:{}]",
                                remote_endpoint_.address().to_string(),
                                remote_endpoint_.port());
}

void session::abort()
{
    if (abort_)
        return;
    abort_ = true;

    std::unique_lock<std::mutex> lck(task_mtx_);
    if (task_)
        task_->abort();
}

httplib::net::awaitable<void> session::run()
{
    auto self = shared_from_this();
    for (; !abort_ && task_;) {
        auto&& next_task = co_await task_->then();
        std::unique_lock<std::mutex> lck(task_mtx_);
        task_ = std::move(next_task);
    }
    co_return;
}
} // namespace httplib