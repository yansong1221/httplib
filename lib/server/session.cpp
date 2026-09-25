#include "session.hpp"
#include "compress/compressor.hpp"
#include "html/accept_content.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include "request_impl.hpp"
#include "response_impl.hpp"
#include "websocket_conn_impl.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/detect_ssl.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/websocket/rfc6455.hpp>

namespace httplib::server
{

    namespace detail
    {

        template <typename S1, typename S2>
        net::awaitable<void>
        transfer(S1& from, S2& to, size_t& bytes_transferred)
        {
            bytes_transferred = 0;
            uint8_t buffer[8196] = { 0 };
            boost::system::error_code ec;

            for (;;)
            {
                auto bytes = co_await from.async_read_some(net::buffer(buffer), util::net_awaitable[ec]);
                if (ec || bytes == 0)
                {
                    if (bytes > 0)
                    {
                        co_await net::async_write(to, net::buffer(buffer, bytes), util::net_awaitable[ec]);
                    }

                    to.shutdown(net::socket_base::shutdown_send, ec);
                    co_return;
                }
                co_await net::async_write(to, net::buffer(buffer, bytes), util::net_awaitable[ec]);
                if (ec)
                {
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
                                    std::shared_ptr<http_server::impl> server_impl)
            : server_impl_(std::move(server_impl))
            , stream_(std::move(stream))
            , buffer_(std::move(buffer))
        {
        }
        ~ssl_handshake_task() {}

        net::awaitable<task::ptr>
        then() override
        {
            boost::system::error_code ec;
            beast::get_lowest_layer(stream_).expires_after(server_impl_->read_timeout());
            auto bytes_used
                = co_await stream_.async_handshake(ssl::stream_base::server, buffer_.data(), util::net_awaitable[ec]);
            beast::get_lowest_layer(stream_).expires_never();
            if (ec)
            {
                server_impl_->get_logger()->trace("ssl handshake failed: {}", ec.message());
                co_return nullptr;
            }
            buffer_.consume(bytes_used);

            http_stream variant_stream(std::move(stream_));
            co_return std::make_shared<http_task>(std::move(variant_stream), std::move(buffer_), server_impl_);
        }

        void
        abort() override
        {
            boost::system::error_code ec;
            stream_.shutdown(ec);
        }

      private:
        std::shared_ptr<http_server::impl> server_impl_;
        http_stream::tls_stream stream_;
        beast::flat_buffer buffer_;
    };
#endif

    session::session(net::any_io_executor ex, tcp::socket&& stream, std::shared_ptr<http_server::impl> server_impl)
        : executor_(std::move(ex))
        , task_(std::make_shared<detect_ssl_task>(std::move(stream), std::move(server_impl)))
    {
    }

    session::~session() {}

    void
    session::abort()
    {
        if (abort_.exchange(true))
        {
            return;
        }
        // 不能从调用线程直接 cancel/close 连接对象；投递回本连接的 strand，
        // 与 run()/then() 的 I/O 串行执行，避免跨线程竞争。
        net::post(executor_,
                  [self = shared_from_this()]()
                  {
                      if (auto t = self->task_)
                      {
                          t->abort();
                      }
                  });
    }

    httplib::net::awaitable<void>
    session::run()
    {
        // run() 在 executor_（strand）上执行，task_ 只在此处和 abort() 投递的回调中访问。
        for (; !abort_;)
        {
            auto t = task_;
            if (!t)
            {
                break;
            }
            auto next_t = co_await t->then();
            task_ = std::move(next_t);
        }
        task_.reset();
        co_return;
    }

    session::detect_ssl_task::detect_ssl_task(tcp::socket&& stream, std::shared_ptr<http_server::impl> server_impl)
        : server_impl_(std::move(server_impl))
        , stream_(std::move(stream))
    {
    }
    session::detect_ssl_task::~detect_ssl_task() {}
    net::awaitable<session::task::ptr>
    session::detect_ssl_task::then()
    {
        beast::flat_buffer buffer;
#ifdef HTTPLIB_ENABLED_SSL
        if (auto ssl_ctx = server_impl_->ssl_context(); ssl_ctx)
        {
            boost::system::error_code ec;
            stream_.expires_after(server_impl_->read_timeout());
            bool is_ssl = co_await beast::async_detect_ssl(stream_, buffer, util::net_awaitable[ec]);
            stream_.expires_never();
            if (ec)
            {
                server_impl_->get_logger()->trace("async_detect_ssl failed: {}", ec.message());
                co_return nullptr;
            }
            if (is_ssl)
            {
                co_return std::make_shared<session::ssl_handshake_task>(
                    http_stream::tls_stream(std::move(stream_), ssl_ctx),
                    std::move(buffer),
                    server_impl_);
            }
        }
#endif
        co_return std::make_shared<session::http_task>(http_stream(std::move(stream_)),
                                                       std::move(buffer),
                                                       server_impl_);
    }
    void
    session::detect_ssl_task::abort()
    {
        boost::system::error_code ec;
        stream_.socket().close(ec);
    }

    session::http_task::http_task(http_stream&& stream,
                                  beast::flat_buffer&& buffer,
                                  std::shared_ptr<http_server::impl> server_impl)
        : server_impl_(std::move(server_impl))
        , buffer_(std::move(buffer))
        , stream_(std::move(stream))
    {
    }

    std::chrono::steady_clock::duration
    session::http_task::read_timeout() const
    {
        return server_impl_->read_timeout();
    }
    std::uint64_t
    session::http_task::body_limit() const
    {
        return server_impl_->body_limit();
    }
    httplib::form_data::param
    session::http_task::form_data_params() const
    {
        return server_impl_->form_data_params();
    }

    std::chrono::steady_clock::duration
    session::http_task::write_timeout() const
    {
        return server_impl_->write_timeout();
    }

    net::awaitable<session::task::ptr>
    session::http_task::then()

    {
        boost::system::error_code ec;
        auto& _router = server_impl_->router();

        auto local_endp = stream_.socket().local_endpoint(ec);
        auto remote_endp = stream_.socket().remote_endpoint(ec);

        auto log_endp_format = fmt::format("({}:{} -> {}:{})",
                                           remote_endp.address().to_string(),
                                           remote_endp.port(),
                                           local_endp.address().to_string(),
                                           local_endp.port());

        for (;;)
        {
            auto header_parser = std::make_unique<http::request_parser<http::empty_body>>();
            header_parser->header_limit(server_impl_->header_limit());
            header_parser->body_limit(server_impl_->body_limit());

            co_await read_header(*header_parser, ec);
            if (ec)
            {
                server_impl_->get_logger()->trace("read http header failed: {}", ec.message());
                co_return nullptr;
            }
            auto start_time = std::chrono::steady_clock::now();
            auto self = std::static_pointer_cast<http_task>(shared_from_this());

            auto req = request::impl::make_request(self,
                                                   local_endp,
                                                   remote_endp,
                                                   std::move(header_parser),
                                                   stream_.is_ssl());

            if (websocket::is_upgrade(get_impl(req).header()))
            {
                server_impl_->get_logger()->trace("ws upgrade {}", req.target());
                co_return std::make_shared<websocket_task>(websocket_stream(std::move(stream_)),
                                                           std::move(req),
                                                           server_impl_);
            }
            auto resp = response::impl::create(get_impl(req).header().version(), get_impl(req).keep_alive(), self);

            auto h_start = std::chrono::steady_clock::time_point {};
            auto handler_ms = std::chrono::milliseconds::zero();

            try
            {
                auto match = co_await _router.pre_routing(req);
                if (!match.node)
                {
                    switch (req.method())
                    {
                        case http::verb::get:
                        case http::verb::head:
                        case http::verb::options:
                        case http::verb::trace:
                            break;
                        default:
                            get_impl(resp).keep_alive(false);
                            break;
                    }
                    if (!match.allows.empty())
                    {
                        resp.set(http::field::allow, boost::join(match.allows, ","));
                        resp.set_error_content(httplib::http::status::method_not_allowed);
                    }
                    else
                    {
                        resp.set_error_content(httplib::http::status::not_found);
                    }
                    server_impl_->get_logger()->debug("{} {} {} {} not matched",
                                                      req.method_string(),
                                                      req.target(),
                                                      resp.result_int(),
                                                      log_endp_format);
                }
                else
                {
                    if (req.method() != http::verb::connect)
                    {
                        if (beast::iequals(req[http::field::expect], "100-continue"))
                        {
                            auto cont_resp = response::impl::create(get_impl(req).header().version(), true, self);
                            cont_resp.set_empty_content(http::status::continue_);
                            if (!co_await async_write(req, cont_resp))
                            {
                                co_return nullptr;
                            }
                        }

                        // make_request 已把 header_parser 归入 lazy 读取栈：
                        // 非 lazy 路由立即全量读入并物化（等同 read_body），
                        // lazy 路由则留给处理函数按需流式读取。
                        if (!match.lazy)
                        {
                            boost::system::error_code ec = co_await req.read_body();
                            if (ec)
                            {
                                server_impl_->get_logger()->trace("read http body failed: {}", ec.message());
                                co_return nullptr;
                            }
                        }
                    }

                    h_start = std::chrono::steady_clock::now();
                    co_await _router.process_routing(match, req, resp);
                    handler_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()
                                                                                       - h_start);
                }
                co_await _router.post_routing(req, resp);

                if (!req.is_body_done())
                {
                    get_impl(resp).keep_alive(false);
                }
            }
            catch (std::exception const& e)
            {
                server_impl_->get_logger()->error("exception in handler for {} {} {}: {}",
                                                  req.method_string(),
                                                  req.target(),
                                                  log_endp_format,
                                                  e.what());
                get_impl(resp).keep_alive(false);
                resp.set_error_content(http::status::internal_server_error);
            }
            catch (...)
            {
                server_impl_->get_logger()->error("unknown exception in handler for {} {} {}",
                                                  req.method_string(),
                                                  req.target(),
                                                  log_endp_format);
                get_impl(resp).keep_alive(false);
                resp.set_error_content(http::status::internal_server_error);
            }

            if (req.method() == http::verb::connect)
            {
                // 放行(<300)进入隧道；否则回写拒绝响应后结束本会话（不再继续读下一个请求）。
                if (resp.result_int() < 300)
                {
                    co_return std::make_unique<http_proxy_task>(std::move(stream_), std::move(req), server_impl_);
                }
                get_impl(resp).keep_alive(false);
            }

            if (!co_await async_write(req, resp))
            {
                co_return nullptr;
            }

            auto total_ms
                = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time);
            using namespace std::chrono_literals;
            server_impl_->get_logger()->debug("{} {} {} {} handler={}ms total={}ms",
                                              req.method_string(),
                                              req.target(),
                                              resp.result_int(),
                                              log_endp_format,
                                              handler_ms.count(),
                                              total_ms.count());

            if (!get_impl(resp).keep_alive())
            {
                co_await stream_.async_graceful_close();
                co_return nullptr;
            }
        }
        co_return nullptr;
    }

    void
    session::http_task::abort()
    {
        boost::system::error_code ec;
        stream_.close(ec);
    }

    net::awaitable<bool>
    session::http_task::async_write(request const& req, response& resp)
    {
        auto& writer = get_impl(resp).writer();
        if (writer.header_sent())
        {
            co_return true;
        }

        writer.prepare_payload();

        if (auto accept_encoding = req[http::field::accept_encoding]; !accept_encoding.empty())
        {
            html::accept_encoding_content encoding_content;
            if (encoding_content.parse(accept_encoding))
            {
                if (auto encoding = encoding_content.server_apply_encoding(); !encoding.empty())
                {
                    if (auto content_type = resp[http::field::content_type];
                        (*server_impl_).should_compress_content_type(content_type))
                    {
                        resp.set(http::field::content_encoding, encoding);
                        writer.apply_encoding(encoding);
                        writer.chunked(true);
                    }
                }
            }
        }
        if (req.method() == http::verb::head)
        {
            writer.reset();
        }

        auto ec = co_await writer.write_message();
        if (ec)
        {
            server_impl_->get_logger()->trace("write http body failed: {}", ec.message());
            co_return false;
        }
        co_return true;
    }

    session::websocket_task::websocket_task(websocket_stream&& stream,
                                            request&& req,
                                            std::shared_ptr<http_server::impl> server_impl)
        : conn_(std::make_shared<websocket_conn_impl>(server_impl, std::move(stream), std::move(req)))
    {
    }

    httplib::net::awaitable<session::task::ptr>
    session::websocket_task::then()
    {
        co_await conn_->run();
        co_return nullptr;
    }

    void
    session::websocket_task::abort()
    {
        conn_->abort();
    }

    session::http_proxy_task::http_proxy_task(http_stream&& stream,
                                              request&& req,
                                              std::shared_ptr<http_server::impl> server_impl)
        : stream_(std::move(stream))
        , req_(std::move(req))
        , server_impl_(std::move(server_impl))
        , resolver_(stream_.get_executor())
        , proxy_socket_(stream_.get_executor())
    {
    }

    net::awaitable<session::task::ptr>
    session::http_proxy_task::then()
    {
        auto target = req_.target();
        auto pos = target.find(":");
        if (pos == std::string_view::npos || pos == target.size() - 1)
        {
            server_impl_->get_logger()->trace("http_proxy: invalid target: {}", target);
            co_return nullptr;
        }

        auto host = target.substr(0, pos);
        auto port = target.substr(pos + 1);

        boost::system::error_code ec;
        auto results = co_await resolver_.async_resolve(host, port, util::net_awaitable[ec]);
        if (ec)
        {
            server_impl_->get_logger()->trace("http_proxy: resolve failed {}: {}", host, ec.message());
            co_return nullptr;
        }

        co_await net::async_connect(proxy_socket_, results, util::net_awaitable[ec]);
        if (ec)
        {
            server_impl_->get_logger()->trace("http_proxy: connect failed {}: {}", host, ec.message());
            co_return nullptr;
        }

        auto resp = response::impl::create(get_impl(req_).header().version(), get_impl(req_).keep_alive(), nullptr);
        get_impl(resp).reason("Connection Established");
        get_impl(resp).result(http::status::ok);
        get_impl(resp).content_length(0);
        httplib::detail::body_writer<false, http_proxy_task> writer(get_impl(resp), this, stream_.get_executor());
        writer.set_empty();
        ec = co_await writer.write_message();
        if (ec)
        {
            server_impl_->get_logger()->trace("http_proxy: write response failed: {}", ec.message());
            co_return nullptr;
        }

        // proxy
        using namespace net::experimental::awaitable_operators;
        size_t l2r_transferred = 0;
        size_t r2l_transferred = 0;
        co_await (detail::transfer(stream_, proxy_socket_, l2r_transferred)
                  && detail::transfer(proxy_socket_, stream_, r2l_transferred));
        co_return nullptr;
    }

    void
    session::http_proxy_task::abort()
    {
        boost::system::error_code ec;
        stream_.close(ec);
        resolver_.cancel();
        proxy_socket_.close(ec);
    }

} // namespace httplib::server
