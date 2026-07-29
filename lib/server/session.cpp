#include "session.hpp"
#include "util/compressor.hpp"
#include "httplib/html/accept_content.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include "request_impl.hpp"
#include "response_impl.hpp"
#include "util/chunk_writer_impl.hpp"
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


namespace httplib::server {

namespace detail {

template<typename S1, typename S2>
net::awaitable<void> transfer(S1& from, S2& to, size_t& bytes_transferred)
{
    static constexpr int buffer_size = 512 * 1024;

    bytes_transferred = 0;
    std::vector<uint8_t> buffer(buffer_size);
    boost::system::error_code ec;

    for (;;) {
        auto bytes = co_await from.async_read_some(net::buffer(buffer), util::net_awaitable[ec]);
        if (ec || bytes == 0) {
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
                                http_server::impl& serv)
        : serv_(serv)
        , stream_(std::move(stream))
        , buffer_(std::move(buffer))
    {
    }
    ~ssl_handshake_task() { }

    net::awaitable<task::ptr> then() override
    {
        boost::system::error_code ec;
        beast::get_lowest_layer(stream_).expires_after(serv_.read_timeout());
        auto bytes_used = co_await stream_.async_handshake(
            ssl::stream_base::server, buffer_.data(), util::net_awaitable[ec]);
        beast::get_lowest_layer(stream_).expires_never();
        if (ec) {
            serv_.logger()->trace("ssl handshake failed: {}", ec.message());
            co_return nullptr;
        }
        buffer_.consume(bytes_used);

        http_stream variant_stream(std::move(stream_));
        co_return std::make_unique<http_task>(std::move(variant_stream), std::move(buffer_), serv_);
    }


    void abort() override { stream_.shutdown(); }

private:
    http_server::impl& serv_;
    http_stream::tls_stream stream_;
    beast::flat_buffer buffer_;
};
#endif

session::session(tcp::socket&& stream, http_server::impl& serv)
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

    std::lock_guard<std::mutex> lck(task_mtx_);
    if (task_)
        task_->abort();
}

httplib::net::awaitable<void> session::run()
{
    for (; !abort_ && task_;) {
        auto&& next_task = co_await task_->then();
        std::lock_guard<std::mutex> lck(task_mtx_);
        task_ = std::move(next_task);
    }
    co_return;
}

session::detect_ssl_task::detect_ssl_task(tcp::socket&& stream, http_server::impl& serv)
    : serv_(serv)
    , stream_(std::move(stream))
{
}
session::detect_ssl_task::~detect_ssl_task()
{
}
net::awaitable<session::task::ptr> session::detect_ssl_task::then()
{
    beast::flat_buffer buffer;
#ifdef HTTPLIB_ENABLED_SSL
    if (auto ssl_ctx = serv_.ssl_context(); ssl_ctx) {
        boost::system::error_code ec;
        stream_.expires_after(serv_.read_timeout());
        bool is_ssl = co_await beast::async_detect_ssl(stream_, buffer, util::net_awaitable[ec]);
        stream_.expires_never();
        if (ec) {
            serv_.logger()->trace("async_detect_ssl failed: {}", ec.message());
            co_return nullptr;
        }
        if (is_ssl) {
            co_return std::make_unique<session::ssl_handshake_task>(
                http_stream::tls_stream(std::move(stream_), ssl_ctx), std::move(buffer), serv_);
        }
    }
#endif
    co_return std::make_unique<session::http_task>(
        http_stream(std::move(stream_)), std::move(buffer), serv_);
}
void session::detect_ssl_task::abort()
{
    stream_.close();
}

session::http_task::http_task(http_stream&& stream,
                              beast::flat_buffer&& buffer,
                              http_server::impl& serv)
    : serv_(serv)
    , buffer_(std::move(buffer))
    , stream_(std::move(stream))
{
}

static net::awaitable<bool>
co_read_normal_body(http_stream& stream,
                    beast::flat_buffer& buffer,
                    http::request_parser<http::empty_body>&& header_parser,
                    request& req,
                    const http_server::impl& serv)
{
    boost::system::error_code ec;
    http::request_parser<body::any_body> body_parser(std::move(header_parser));

    if (!serv.upload_dir().empty()) {
        auto ct = body_parser.get()[http::field::content_type];
        if (ct.starts_with("multipart/form-data")) {
            auto& body       = body_parser.get().body();
            body             = body::form_data_body::value_type {};
            auto& fd         = std::get<body::form_data_body::value_type>(body);
            fd.save_dir      = serv.upload_dir();
            fd.max_file_size = serv.upload_file_limit();
        }
    }

    while (!body_parser.is_done()) {
        stream.expires_after(serv.read_timeout());
        co_await http::async_read_some(stream, buffer, body_parser, util::net_awaitable[ec]);
        if (ec) {
            stream.expires_never();
            serv.logger()->trace("read http body failed: {}", ec.message());
            co_return false;
        }
    }
    stream.expires_never();
    req.get_impl()->body() = std::move(body_parser.release().body());
    co_return true;
}

net::awaitable<session::task::ptr> session::http_task::then()

{
    boost::system::error_code ec;
    auto& _router = serv_.router();

    auto local_endp  = stream_.socket().local_endpoint(ec);
    auto remote_endp = stream_.socket().remote_endpoint(ec);

    auto log_endp_format = fmt::format("{}:{} -> {}:{}",
                                       remote_endp.address().to_string(),
                                       remote_endp.port(),
                                       local_endp.address().to_string(),
                                       local_endp.port());

    for (;;) {
        http::request_parser<http::empty_body> header_parser;
        header_parser.header_limit(std::numeric_limits<std::uint32_t>::max());
        header_parser.body_limit(std::numeric_limits<unsigned long long>::max());

        stream_.expires_after(serv_.read_timeout());
        co_await http::async_read_header(stream_, buffer_, header_parser, util::net_awaitable[ec]);
        stream_.expires_never();
        if (ec) {
            serv_.logger()->trace("read http header failed: {}", ec.message());
            co_return nullptr;
        }

        auto start_time = std::chrono::steady_clock::now();

        const auto& header = header_parser.get();
        serv_.logger()->trace(
            "{} {} ({})", header.method_string(), header.target(), log_endp_format);

        // http proxy
        if (header.method() == http::verb::connect) {
            auto req = request::impl::make_request(
                local_endp, remote_endp, std::move(header_parser.release()));
            co_return std::make_unique<http_proxy_task>(std::move(stream_), std::move(req), serv_);
        }
        // websocket
        if (websocket::is_upgrade(header.base())) {
            auto req = request::impl::make_request(
                local_endp, remote_endp, std::move(header_parser.release()));
            co_return std::make_unique<websocket_task>(
                websocket_stream(std::move(stream_)), std::move(req), serv_);
        }

        auto resp = response::impl::make_response(header.version(), header.keep_alive());
        auto req  = request::impl::make_request(
            local_endp, remote_endp, http::request<http::empty_body>(header));

        auto h_start    = std::chrono::steady_clock::time_point {};
        auto handler_ms = std::chrono::milliseconds::zero();

        auto record_handler_time = [&] {
            if (handler_ms == std::chrono::milliseconds::zero() &&
                h_start != std::chrono::steady_clock::time_point {})
                handler_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - h_start);
        };

        try {
            auto match = co_await _router.pre_routing(req);
            if (!match.node) {
                resp.get_impl()->keep_alive(false);
                _router.write_error(resp, match.allows);
            }
            else {
                if (beast::iequals(header[http::field::expect], "100-continue")) {
                    auto cont_resp = response::impl::make_response(header.version(), true);
                    cont_resp.set_empty_content(http::status::continue_);
                    if (!co_await async_write(req, cont_resp))
                        co_return nullptr;
                }

                if (match.body == router_impl::body_kind::buffer_body) {
                    req.get_impl()->setup_buffer_body_reading(
                        stream_, buffer_, std::move(header_parser), serv_.read_timeout());
                }
                else if (match.body == router_impl::body_kind::chunked) {
                    req.get_impl()->setup_chunked_reading(
                        stream_, buffer_, std::move(header_parser), serv_.read_timeout());
                }
                else {
                    if (!co_await co_read_normal_body(
                            stream_, buffer_, std::move(header_parser), req, serv_))
                        co_return nullptr;
                }

                h_start = std::chrono::steady_clock::now();
                co_await _router.process_routing(match, req, resp);
                handler_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - h_start);
            }
            co_await _router.post_routing(req, resp);
        }
        catch (const std::exception& e) {
            record_handler_time();
            serv_.logger()->warn("exception in business function, reason: {}", e.what());
            resp.set_string_content(
                std::string(e.what()), "text/plain", http::status::internal_server_error);
        }
        catch (...) {
            record_handler_time();
            using namespace std::string_view_literals;
            serv_.logger()->warn("unknown exception in business function");
            resp.set_string_content(std::string("unknown exception"),
                                    "text/plain",
                                    http::status::internal_server_error);
        }

        if (!co_await async_write(req, resp))
            co_return nullptr;

        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);

        using namespace std::chrono_literals;
        serv_.logger()->log(handler_ms > 10s ? spdlog::level::warn : spdlog::level::debug,
                            "{} {} ({}) {} handler={}ms total={}ms",
                            req.method_string(),
                            req.target(),
                            log_endp_format,
                            resp.result_int(),
                            handler_ms.count(),
                            total_ms.count());

        if (!resp.get_impl()->keep_alive()) {
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

net::awaitable<bool> session::http_task::async_write(const request& req, response& resp)
{
    if (resp.get_impl()->chunked_write_handler_) {
        resp.get_impl()->chunked(true);
    }
    else {
        if (!resp.get_impl()->has_content_length())
            resp.get_impl()->prepare_payload();

        if (auto accept_encoding = req[http::field::accept_encoding]; !accept_encoding.empty()) {
            html::accept_encoding_content encoding_content;
            if (encoding_content.parse(accept_encoding)) {
                if (auto encoding = encoding_content.server_apply_encoding(); !encoding.empty()) {
                    if (auto content_type = resp[http::field::content_type];
                        serv_.should_compress_content_type(content_type))
                    {
                        resp.set(http::field::content_encoding, encoding);
                        resp.get_impl()->chunked(true);
                    }
                }
            }
        }
    }
    if (req.method() == http::verb::head)
        resp.get_impl()->reset_content();

    boost::system::error_code ec;
    http::response_serializer<body::any_body> serializer((*resp.get_impl()));
    {
        auto has_body_handler = resp.get_impl()->chunked_write_handler_ != nullptr;
        if (has_body_handler)
            serializer.split(true);

        while (has_body_handler ? !serializer.is_header_done()
                                : !serializer.is_done())
        {
            stream_.expires_after(serv_.write_timeout());
            co_await http::async_write_some(stream_, serializer, util::net_awaitable[ec]);
            if (ec) {
                stream_.expires_never();
                serv_.logger()->trace("write http body failed: {}", ec.message());
                co_return false;
            }
        }
        stream_.expires_never();
    }

    if (auto handler = resp.get_impl()->chunked_write_handler_; handler) {
        auto writer = std::make_unique<chunk_writer_impl>(stream_, serv_.write_timeout());
        co_await handler(*writer);
        co_await writer->close();
    }
    co_return true;
}

session::websocket_task::websocket_task(websocket_stream&& stream,
                                        request&& req,
                                        http_server::impl& serv)
    : conn_(std::make_shared<websocket_conn_impl>(serv, std::move(stream), std::move(req)))
{
}

httplib::net::awaitable<session::task::ptr> session::websocket_task::then()
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
                                          http_server::impl& serv)
    : stream_(std::move(stream))
    , req_(std::move(req))
    , serv_(serv)
    , resolver_(stream_.get_executor())
    , proxy_socket_(stream_.get_executor())
{
}

net::awaitable<session::task::ptr> session::http_proxy_task::then()
{
    auto target = req_.target();
    auto pos    = target.find(":");
    if (pos == std::string_view::npos || pos == target.size() - 1) {
        serv_.logger()->trace("http_proxy: invalid target: {}", target);
        co_return nullptr;
    }

    auto host = target.substr(0, pos);
    auto port = target.substr(pos + 1);

    boost::system::error_code ec;
    auto results = co_await resolver_.async_resolve(host, port, util::net_awaitable[ec]);
    if (ec) {
        serv_.logger()->trace("http_proxy: resolve failed {}: {}", host, ec.message());
        co_return nullptr;
    }

    co_await net::async_connect(proxy_socket_, results, util::net_awaitable[ec]);
    if (ec) {
        serv_.logger()->trace("http_proxy: connect failed {}: {}", host, ec.message());
        co_return nullptr;
    }

    auto resp =
        response::impl::make_response(req_.get_impl()->version(), req_.get_impl()->keep_alive());
    resp.get_impl()->reason("Connection Established");
    resp.get_impl()->result(http::status::ok);
    co_await http::async_write(stream_, (*resp.get_impl()), util::net_awaitable[ec]);
    if (ec) {
        serv_.logger()->trace("http_proxy: write response failed: {}", ec.message());
        co_return nullptr;
    }

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
