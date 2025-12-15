#include "session.hpp"
#include "body/compressor.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
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
#ifdef HTTPLIB_ENABLED_SSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif


namespace httplib::server {

namespace detail {

#ifdef HTTPLIB_ENABLED_SSL

static std::shared_ptr<ssl::context> create_ssl_context(const net::const_buffer& cert_file,
                                                        const net::const_buffer& key_file,
                                                        std::string passwd,
                                                        boost::system::error_code& ec)
{
    unsigned long ssl_options =
        ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::single_dh_use;

    auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
    ssl_ctx->set_options(ssl_options, ec);
    if (ec)
        return nullptr;

    if (!passwd.empty()) {
        ssl_ctx->set_password_callback(
            [pass = std::move(passwd)](auto, auto) {
                if (pass.empty())
                    throw std::runtime_error("ssl password is empty!");
                return pass;
            },
            ec);
        if (ec)
            return nullptr;
    }
    ssl_ctx->use_certificate(cert_file, ssl::context_base::pem, ec);
    if (ec)
        return nullptr;

    ssl_ctx->use_rsa_private_key(key_file, ssl::context::pem, ec);
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
        auto bytes = co_await from.async_read_some(net::buffer(buffer), util::net_awaitable[ec]);
        if (ec) {
            if (bytes > 0)
                co_await net::async_write(to, net::buffer(buffer, bytes), util::net_awaitable[ec]);

            to.shutdown(net::socket_base::shutdown_send, ec);
            co_return;
        }
        co_await net::async_write(to, net::buffer(buffer, bytes), util::net_awaitable[ec]);
        if (ec) {
            from.shutdown(net::socket_base::shutdown_receive, ec);
            co_return;
        }
        bytes_transferred += bytes;
    }
}


} // namespace detail


#ifdef HTTPLIB_ENABLED_SSL
class session::ssl_handshake_task : public session::task
{
public:
    explicit ssl_handshake_task(http_stream::tls_stream&& stream,
                                beast::flat_buffer&& buffer,
                                http_server_impl& serv)
        : serv_(serv)
        , stream_(std::move(stream))
        , buffer_(std::move(buffer))
    {
    }


    net::awaitable<std::unique_ptr<task>> then() override
    {
        boost::system::error_code ec;
        auto bytes_used = co_await stream_.async_handshake(
            ssl::stream_base::server, buffer_.data(), util::net_awaitable[ec]);
        if (ec) {
            serv_.get_logger()->trace("ssl handshake failed: {}", ec.message());
            co_return nullptr;
        }
        buffer_.consume(bytes_used);

        http_stream variant_stream(std::move(stream_));
        co_return std::make_unique<http_task>(std::move(variant_stream), std::move(buffer_), serv_);
    }


    void abort() override { stream_.shutdown(); }

private:
    http_server_impl& serv_;
    http_stream::tls_stream stream_;
    beast::flat_buffer buffer_;
};
#endif

session::session(tcp::socket&& stream, http_server_impl& serv)
    : task_(std::make_unique<detect_ssl_task>(std::move(stream), serv))
{
}

session::~session()
{
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
    for (; !abort_ && task_;) {
        auto&& next_task = co_await task_->then();
        std::unique_lock<std::mutex> lck(task_mtx_);
        task_ = std::move(next_task);
    }
    co_return;
}

session::detect_ssl_task::detect_ssl_task(tcp::socket&& stream, http_server_impl& sevr)
    : sevr_(sevr)
    , stream_(std::move(stream))
{
    stream_.expires_after(sevr_.read_timeout());
}
session::detect_ssl_task::~detect_ssl_task()
{
    stream_.expires_never();
}
net::awaitable<std::unique_ptr<session::task>> session::detect_ssl_task::then()
{
    beast::flat_buffer buffer;
#ifdef HTTPLIB_ENABLED_SSL
    if (sevr_.ssl_conf_) {
        boost::system::error_code ec;
        bool is_ssl = co_await beast::async_detect_ssl(stream_, buffer, util::net_awaitable[ec]);
        if (ec) {
            sevr_.get_logger()->trace("async_detect_ssl failed: {}", ec.message());
            co_return nullptr;
        }
        if (is_ssl) {
            auto ssl_ctx = detail::create_ssl_context(net::buffer(sevr_.ssl_conf_->cert_file),
                                                      net::buffer(sevr_.ssl_conf_->key_file),
                                                      sevr_.ssl_conf_->passwd,
                                                      ec);
            if (!ssl_ctx) {
                sevr_.get_logger()->error("create_ssl_context failed: {}", ec.message());
                co_return nullptr;
            }
            co_return std::make_unique<session::ssl_handshake_task>(
                http_stream::tls_stream(std::move(stream_), ssl_ctx), std::move(buffer), sevr_);
        }
    }
#endif
    co_return std::make_unique<session::http_task>(
        http_stream(std::move(stream_)), std::move(buffer), sevr_);
}
void session::detect_ssl_task::abort()
{
    stream_.close();
}

session::http_task::http_task(http_stream&& stream,
                              beast::flat_buffer&& buffer,
                              http_server_impl& serv)
    : serv_(serv)
    , buffer_(std::move(buffer))
    , stream_(std::move(stream))
{
    local_endpoint_  = stream_.socket().local_endpoint();
    remote_endpoint_ = stream_.socket().remote_endpoint();
}

httplib::net::awaitable<std::unique_ptr<session::task>> session::http_task::then()

{
    boost::system::error_code ec;
    auto& _router = serv_.router();

    for (;;) {
        // using namespace std::chrono_;
        http::request_parser<http::empty_body> header_parser;
        header_parser.header_limit(std::numeric_limits<std::uint32_t>::max());
        header_parser.body_limit(std::numeric_limits<unsigned long long>::max());

        stream_.expires_after(serv_.read_timeout());
        co_await http::async_read_header(stream_, buffer_, header_parser, util::net_awaitable[ec]);
        stream_.expires_never();
        if (ec) {
            serv_.get_logger()->trace("read http header failed: {}", ec.message());
            co_return nullptr;
        }

        const auto& header = header_parser.get();

        // http proxy
        if (header.method() == http::verb::connect) {
            request req(local_endpoint_, remote_endpoint_, std::move(header_parser.release()));
            co_return std::make_unique<http_proxy_task>(std::move(stream_), std::move(req), serv_);
        }
        // websocket
        if (websocket::is_upgrade(header.base())) {
            auto stream = websocket_stream::create(std::move(stream_));
            request req(local_endpoint_, remote_endpoint_, std::move(header_parser.release()));
            co_return std::make_unique<websocket_task>(std::move(stream), std::move(req), serv_);
        }

        response resp(header.version(), header.keep_alive());
        request req(local_endpoint_, remote_endpoint_, http::request<http::empty_body>(header));

        auto start_time = std::chrono::steady_clock::now();

        try {
            if (co_await _router.pre_routing(req, resp)) {
                if (beast::iequals(header[http::field::expect], "100-continue")) {
                    // send 100 response
                    response resp(header.version(), true);
                    resp.set_empty_content(http::status::continue_);
                    if (!co_await async_write(req, resp))
                        co_return nullptr;
                }

                http::request_parser<body::any_body> body_parser(std::move(header_parser));
                while (!body_parser.is_done()) {
                    stream_.expires_after(serv_.read_timeout());
                    co_await http::async_read_some(
                        stream_, buffer_, body_parser, util::net_awaitable[ec]);
                    stream_.expires_never();
                    if (ec) {
                        serv_.get_logger()->trace("read http body failed: {}", ec.message());
                        co_return nullptr;
                    }
                }
                req.body() = std::move(body_parser.release().body());
                start_time = std::chrono::steady_clock::now();

                co_await _router.proc_routing(req, resp);
            }
            co_await _router.post_routing(req, resp);
        }
        catch (const std::exception& e) {
            serv_.get_logger()->warn("exception in business function, reason: {}", e.what());
            resp.set_string_content(
                std::string(e.what()), "text/plain", http::status::internal_server_error);
        }
        catch (...) {
            using namespace std::string_view_literals;
            serv_.get_logger()->warn("unknown exception in business function");
            resp.set_string_content(std::string("unknown exception"),
                                    "text/plain",
                                    http::status::internal_server_error);
        }

        auto span_time = std::chrono::steady_clock::now() - start_time;

        serv_.get_logger()->debug(
            "{} {} ({}:{} -> {}:{}) {} {}ms",
            req.method_string(),
            req.target(),
            remote_endpoint_.address().to_string(),
            remote_endpoint_.port(),
            local_endpoint_.address().to_string(),
            local_endpoint_.port(),
            resp.result_int(),
            std::chrono::duration_cast<std::chrono::milliseconds>(span_time).count());


        if (!co_await async_write(req, resp))
            co_return nullptr;

        if (!resp.keep_alive()) {
            boost::system::error_code ec;
            // This means we should close the connection, usually
            // because the response indicated the "Connection: close"
            // semantic.
            stream_.close();
            co_return nullptr;
        }
    }
    co_return nullptr;
}

void session::http_task::abort()
{
    stream_.close();
}

net::awaitable<bool> session::http_task::async_write(request& req, response& resp)
{
    if (resp.stream_handler_) {
        resp.chunked(true);
    }
    else {
        if (!resp.has_content_length())
            resp.prepare_payload();

        if (auto iter = req.find(http::field::accept_encoding); iter != req.end()) {
            auto accept_encodings = util::split(iter->value(), ",");
            for (const auto& encoding : accept_encodings) {
                if (httplib::body::compressor_factory::instance().is_supported_encoding(encoding)) {
                    resp.set(http::field::content_encoding, encoding);
                    resp.chunked(true);
                    break;
                }
            }
        }
    }

    boost::system::error_code ec;
    http::response_serializer<body::any_body> serializer(resp);
    stream_.expires_after(serv_.write_timeout());
    co_await http::async_write_header(stream_, serializer, util::net_awaitable[ec]);
    stream_.expires_never();
    if (ec) {
        serv_.get_logger()->trace("write http header failed: {}", ec.message());
        co_return false;
    }

    if (req.method() == http::verb::head)
        co_return true;

    if (resp.stream_handler_) {
        for (;;) {
            bool has_more = co_await resp.stream_handler_(buffer_, ec);
            if (ec) {
                serv_.get_logger()->trace("read chunk body failed: {}", ec.message());
                co_return false;
            }
            if (buffer_.size() != 0) {
                http::chunk_body chunk_b(buffer_.data());
                stream_.expires_after(serv_.write_timeout());
                co_await net::async_write(stream_, chunk_b, util::net_awaitable[ec]);
                stream_.expires_never();
                if (ec) {
                    serv_.get_logger()->trace("write chunk body failed: {}", ec.message());
                    co_return false;
                }
                buffer_.consume(buffer_.size());
            }
            if (!has_more) {
                http::chunk_last chunk_last;
                stream_.expires_after(serv_.write_timeout());
                co_await net::async_write(stream_, chunk_last, util::net_awaitable[ec]);
                stream_.expires_never();
                if (ec) {
                    serv_.get_logger()->trace("write chunk last failed: {}", ec.message());
                    co_return false;
                }
                break;
            }
        }
    }
    else {
        while (!serializer.is_done()) {
            stream_.expires_after(serv_.write_timeout());
            co_await http::async_write_some(stream_, serializer, util::net_awaitable[ec]);
            stream_.expires_never();
            if (ec) {
                serv_.get_logger()->trace("write http body failed: {}", ec.message());
                co_return false;
            }
        }
    }
    co_return true;
}

session::websocket_task::websocket_task(std::unique_ptr<websocket_stream>&& stream,
                                        request&& req,
                                        http_server_impl& serv)
    : conn_(std::make_shared<websocket_conn_impl>(serv, std::move(stream), std::move(req)))
{
}

httplib::net::awaitable<std::unique_ptr<session::task>> session::websocket_task::then()
{
    co_await conn_->run();
    co_return nullptr;
}

void session::websocket_task::abort()
{
    conn_->close();
}

session::http_proxy_task::http_proxy_task(http_stream&& stream,
                                          request&& req,
                                          http_server_impl& serv)
    : stream_(std::move(stream))
    , req_(std::move(req))
    , serv_(serv)
    , resolver_(stream_.get_executor())
    , proxy_socket_(stream_.get_executor())
{
}

httplib::net::awaitable<std::unique_ptr<session::task>> session::http_proxy_task::then()
{
    auto target = req_.target();
    auto pos    = target.find(":");
    if (pos == std::string_view::npos)
        co_return nullptr;

    auto host = target.substr(0, pos);
    auto port = target.substr(pos + 1);

    boost::system::error_code ec;
    auto results = co_await resolver_.async_resolve(host, port, util::net_awaitable[ec]);
    if (ec)
        co_return nullptr;

    co_await net::async_connect(proxy_socket_, results, util::net_awaitable[ec]);
    if (ec)
        co_return nullptr;

    response resp(req_.version(), req_.keep_alive());
    resp.reason("Connection Established");
    resp.result(http::status::ok);
    co_await http::async_write(stream_, resp, util::net_awaitable[ec]);
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

void session::http_proxy_task::abort()
{
    boost::system::error_code ec;
    stream_.close();
    resolver_.cancel();
    proxy_socket_.close(ec);
}

} // namespace httplib::server