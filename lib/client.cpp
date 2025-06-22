#include "httplib/client.hpp"

#include "body/compressor.hpp"
#include "httplib/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>


namespace httplib {
class client::impl
{
public:
    impl(const net::any_io_executor& ex, std::string_view host, uint16_t port)
        : executor_(ex)
        , resolver_(ex)
        , host_(host)
        , port_(port)
    {
    }
    ~impl()
    {
        if (variant_stream_) {
            boost::system::error_code ec;
            variant_stream_->close(ec);
        }
    }

    void set_timeout_policy(const timeout_policy& policy) { timeout_policy_ = policy; }

    void set_timeout(const std::chrono::steady_clock::duration& duration) { timeout_ = duration; }
    void set_use_ssl(bool ssl) { use_ssl_ = ssl; }

    client::request make_http_request(http::verb method,
                                      std::string_view path,
                                      const http::fields& headers)
    {
        auto host = host_;
        if ((use_ssl_ && port_ != 443) || (!use_ssl_ && port_ != 80))
            host += fmt::format(":{}", port_);

        client::request req(method, path, 11);
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::accept, "*/*");

        const auto& encoding = body::compressor_factory::instance().supported_encoding();
        if (!encoding.empty())
            req.set(http::field::accept_encoding, boost::join(encoding, ","));

        for (const auto& field : headers)
            req.set(field.name_string(), field.value());
        req.keep_alive(true);
        return std::move(req);
    }

public:
    void close()
    {
        resolver_.cancel();
        if (variant_stream_) {
            variant_stream_->expires_never();
            boost::system::error_code ec;
            variant_stream_->close(ec);
        }
    }

    bool is_open() const { return variant_stream_ && variant_stream_->is_open(); }

    net::awaitable<client::response_result> async_send_request(client::request& req,
                                                               bool retry = true)
    {
        boost::system::error_code ec;
        try {
            client::response resp = co_await async_send_request_impl(req);
            co_return resp;
        }
        catch (const boost::system::system_error& error) {
            ec = error.code();
        }
        catch (...) {
            ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
        }
        close();

        if (ec == boost::asio::error::connection_aborted ||
            ec == boost::asio::error::connection_reset)
        {
            if (retry)
                co_return co_await async_send_request(req, false);
        }
        co_return ec;
    }

    net::awaitable<client::response> async_send_request_impl(client::request& req)
    {
        auto expires_after = [this](auto& stream, bool first = false) {
            if (timeout_policy_ == timeout_policy::step)
                beast::get_lowest_layer(stream).expires_after(timeout_);
            else if (timeout_policy_ == timeout_policy::never)
                beast::get_lowest_layer(stream).expires_never();
            else if (timeout_policy_ == timeout_policy::overall) {
                if (!first)
                    return;
                beast::get_lowest_layer(stream).expires_after(timeout_);
            }
        };

        // Set up an HTTP GET request message
        if (!is_open()) {
            auto endpoints =
                co_await resolver_.async_resolve(host_, std::to_string(port_), net::use_awaitable);

            if (use_ssl_) {
#ifdef HTTPLIB_ENABLED_SSL
                unsigned long ssl_options = ssl::context::default_workarounds |
                                            ssl::context::no_sslv2 | ssl::context::single_dh_use;

                auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
                ssl_ctx->set_options(ssl_options);
                ssl_ctx->set_default_verify_paths();
                ssl_ctx->set_verify_mode(ssl::verify_none);

                ssl_http_stream stream(co_await net::this_coro::executor, ssl_ctx);
                if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str())) {
                    beast::error_code ec {static_cast<int>(::ERR_get_error()),
                                          net::error::get_ssl_category()};
                    throw boost::system::system_error(ec);
                }
                expires_after(stream, true);
                co_await stream.next_layer().async_connect(endpoints, net::use_awaitable);
                co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

                variant_stream_ =
                    std::make_unique<http_variant_stream_type>(ssl_http_stream(std::move(stream)));
#else
                throw boost::system::system_error(boost::system::errc::make_error_code(
                    boost::system::errc::protocol_not_supported));
#endif
            }
            else {
                http_stream stream(co_await net::this_coro::executor);
                expires_after(stream, true);
                co_await stream.async_connect(endpoints, net::use_awaitable);
                variant_stream_ =
                    std::make_unique<http_variant_stream_type>(http_stream(std::move(stream)));
            }
        }

        http::request_serializer<body::any_body> serializer(req);
        while (!serializer.is_done()) {
            expires_after(*variant_stream_);
            co_await http::async_write_some(*variant_stream_, serializer);
        }

        beast::flat_buffer buffer;

        http::response_parser<http::empty_body> header_parser;
        while (!header_parser.is_header_done()) {
            expires_after(*variant_stream_);
            co_await http::async_read_some(*variant_stream_, buffer, header_parser);
        }

        http::response_parser<body::any_body> body_parser(std::move(header_parser));
        if (req.method() != http::verb::head) {
            while (!body_parser.is_done()) {
                expires_after(*variant_stream_);
                co_await http::async_read_some(*variant_stream_, buffer, body_parser);
            }
        }
        variant_stream_->expires_never();
        co_return body_parser.release();
    }


    net::any_io_executor executor_;
    tcp::resolver resolver_;
    timeout_policy timeout_policy_               = timeout_policy::overall;
    std::chrono::steady_clock::duration timeout_ = std::chrono::seconds(30);
    std::string host_;
    uint16_t port_ = 0;
    std::unique_ptr<http_variant_stream_type> variant_stream_;
    bool use_ssl_ = false;
};

client::client(net::io_context& ex, std::string_view host, uint16_t port)
    : client(ex.get_executor(), host, port)
{
}

client::client(const net::any_io_executor& ex, std::string_view host, uint16_t port)
    : impl_(new client::impl(ex, host, port))
{
}

client::~client()
{
    delete impl_;
}

void client::set_timeout_policy(const timeout_policy& policy)
{
    impl_->set_timeout_policy(policy);
}

void client::set_timeout(const std::chrono::steady_clock::duration& duration)
{
    impl_->set_timeout(duration);
}

void client::set_use_ssl(bool ssl)
{
    impl_->set_use_ssl(ssl);
}

std::string_view client::host() const
{
    return impl_->host_;
}

uint16_t client::port() const
{
    return impl_->port_;
}

bool client::is_use_ssl() const
{
    return impl_->use_ssl_;
}

net::awaitable<client::response_result>
client::async_get(std::string_view path,
                  const html::query_params& params,
                  const http::fields& headers /*= http::fields()*/)
{
    auto query = html::make_http_query_params(params);
    std::string target(path);
    if (!query.empty()) {
        target += "?";
        target += query;
    }
    auto req = impl_->make_http_request(http::verb::get, target, headers);
    co_return co_await impl_->async_send_request(req);
}


httplib::net::awaitable<client::response_result>
client::async_head(std::string_view path, const http::fields& headers /*= http::fields()*/)
{
    auto req = impl_->make_http_request(http::verb::head, path, headers);
    co_return co_await impl_->async_send_request(req);
}

httplib::net::awaitable<client::response_result> client::async_post(
    std::string_view path, std::string_view body, const http::fields& headers /*= http::fields()*/)
{
    auto request = impl_->make_http_request(http::verb::post, path, headers);
    request.content_length(body.size());
    request.body() = std::string(body);
    co_return co_await impl_->async_send_request(request);
}

httplib::net::awaitable<client::response_result>
client::async_post(std::string_view path,
                   boost::json::value&& body,
                   const http::fields& headers /*= http::fields()*/)
{
    auto request = impl_->make_http_request(http::verb::post, path, headers);
    request.set(http::field::content_type, "application/json");
    request.body() = std::move(body);
    request.prepare_payload();
    co_return co_await impl_->async_send_request(request);
}

client::response_result client::get(std::string_view path,
                                    const html::query_params& params,
                                    const http::fields& headers /*= http::fields()*/)
{
    auto future =
        net::co_spawn(impl_->executor_, async_get(path, params, headers), net::use_future);
    return future.get();
}

void client::close()
{
    impl_->close();
}

bool client::is_open() const
{
    return impl_->is_open();
}

} // namespace httplib