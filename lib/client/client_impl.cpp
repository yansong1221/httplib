#include "client_impl.h"
#include "body/compressor.hpp"
#include "httplib/use_awaitable.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>


namespace httplib::client {

http_client::impl::impl(const net::any_io_executor& ex,
                        std::string_view host,
                        uint16_t port,
                        bool ssl)

    : executor_(ex)
    , resolver_(ex)
    , host_(host)
    , port_(port)
    , use_ssl_(ssl)
{
}
http_client::impl::~impl()
{
    close();
}

http_client::request http_client::impl::make_http_request(http::verb method,
                                                          std::string_view path,
                                                          const http::fields& headers)
{
    std::string host;
    if ((use_ssl_ && port_ != 443) || (!use_ssl_ && port_ != 80))
        host += fmt::format("{}:{}", host_, port_);
    else
        host = host_;

    http_client::request req(method, path, 11);
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

void http_client::impl::close()
{
    resolver_.cancel();

    std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
    if (variant_stream_) {
        variant_stream_->expires_never();
        std::visit(
            [](auto&& stream) {
                using stream_type = std::decay_t<decltype(stream)>;
#ifdef HTTPLIB_ENABLED_SSL
                if constexpr (std::is_same_v<stream_type, ssl_http_stream>) {
                    boost::system::error_code ec;
                    stream.shutdown(ec);
                }
#endif
            },
            *variant_stream_);
        boost::system::error_code ec;
        variant_stream_->shutdown(net::socket_base::shutdown_type::shutdown_both, ec);
        variant_stream_->close(ec);
    }
}

bool http_client::impl::is_open() const
{
    std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
    return variant_stream_ && variant_stream_->is_open();
}

net::awaitable<http_client::response_result>
http_client::impl::async_send_request(http_client::request& req, bool retry /*= true*/)
{
    boost::system::error_code ec;
    try {
        http_client::response resp = co_await async_send_request_impl(req);
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
        ec == boost::asio::error::connection_reset || ec == http::error::end_of_stream)
    {
        if (retry)
            co_return co_await async_send_request(req, false);
    }
    co_return ec;
}

void http_client::impl::expires_after(bool first /*= false*/)
{
    std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
    if (!variant_stream_)
        return;

    if (timeout_policy_ == timeout_policy::step)
        variant_stream_->expires_after(timeout_);
    else if (timeout_policy_ == timeout_policy::never)
        variant_stream_->expires_never();
    else if (timeout_policy_ == timeout_policy::overall) {
        if (!first)
            return;
        variant_stream_->expires_after(timeout_);
    }
}

net::awaitable<http_client::response>
http_client::impl::async_send_request_impl(http_client::request& req)
{
    // Set up an HTTP GET request message
    if (!is_open()) {
        auto endpoints =
            co_await resolver_.async_resolve(host_, std::to_string(port_), net::use_awaitable);

        if (use_ssl_) {
#ifdef HTTPLIB_ENABLED_SSL
            unsigned long ssl_options = ssl::context::default_workarounds | ssl::context::no_sslv2 |
                                        ssl::context::single_dh_use;

            auto ssl_ctx = std::make_shared<ssl::context>(ssl::context::sslv23);
            ssl_ctx->set_options(ssl_options);
            ssl_ctx->set_default_verify_paths();
            ssl_ctx->set_verify_mode(ssl::verify_none);

            ssl_http_stream ssl_stream(executor_, ssl_ctx);
            if (!SSL_set_tlsext_host_name(ssl_stream.native_handle(), host_.c_str())) {
                beast::error_code ec {static_cast<int>(::ERR_get_error()),
                                      net::error::get_ssl_category()};
                throw boost::system::system_error(ec);
            }

            std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
            variant_stream_ = std::make_unique<http_variant_stream_type>(std::move(ssl_stream));
#else
            throw boost::system::system_error(
                boost::system::errc::make_error_code(boost::system::errc::protocol_not_supported));
#endif
        }
        else {
            http_stream stream(executor_);
            std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
            variant_stream_ = std::make_unique<http_variant_stream_type>(std::move(stream));
        }

        net::socket_base::reuse_address option(true);
        variant_stream_->lowest_layer().set_option(option);

        expires_after(true);

        co_await std::visit(
            [&](auto&& stream) -> net::awaitable<void> {
                using stream_type = std::decay_t<decltype(stream)>;
#ifdef HTTPLIB_ENABLED_SSL
                if constexpr (std::is_same_v<stream_type, ssl_http_stream>) {
                    co_await stream.next_layer().async_connect(endpoints, net::use_awaitable);
                    co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);
                }
#endif
                if constexpr (std::is_same_v<stream_type, http_stream>) {
                    co_await stream.async_connect(endpoints, net::use_awaitable);
                }
            },
            *variant_stream_);
    }

    http::request_serializer<body::any_body> serializer(req);
    while (!serializer.is_done()) {
        expires_after();
        co_await http::async_write_some(*variant_stream_, serializer);
    }

    beast::flat_buffer buffer;

    http::response_parser<http::empty_body> header_parser;
    while (!header_parser.is_header_done()) {
        expires_after();
        co_await http::async_read_some(*variant_stream_, buffer, header_parser);
    }

    http::response_parser<body::any_body> body_parser(std::move(header_parser));
    if (req.method() != http::verb::head) {
        while (!body_parser.is_done()) {
            expires_after();
            co_await http::async_read_some(*variant_stream_, buffer, body_parser);
        }
    }
    variant_stream_->expires_never();
    if (!body_parser.keep_alive())
        close();
    co_return body_parser.release();
}


} // namespace httplib::client
