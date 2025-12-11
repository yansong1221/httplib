#include "client_impl.h"
#include "body/compressor.hpp"
#include "helper.hpp"
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

    if (!chunk_handler_) {
        const auto& encoding = body::compressor_factory::instance().supported_encoding();
        if (!encoding.empty())
            req.set(http::field::accept_encoding, boost::join(encoding, ","));
    }

    for (const auto& field : headers)
        req.set(field.name_string(), field.value());
    req.keep_alive(true);
    return std::move(req);
}

void http_client::impl::close()
{
    resolver_.cancel();

    std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
    if (stream_) {
        stream_->expires_never();
        stream_->close();
    }
}

bool http_client::impl::is_open() const
{
    std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
    return stream_ && stream_->is_open();
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
    if (!stream_)
        return;

    if (timeout_policy_ == timeout_policy::step)
        stream_->expires_after(timeout_);
    else if (timeout_policy_ == timeout_policy::never)
        stream_->expires_never();
    else if (timeout_policy_ == timeout_policy::overall) {
        if (!first)
            return;
        stream_->expires_after(timeout_);
    }
}

net::awaitable<http_client::response>
http_client::impl::async_send_request_impl(http_client::request& req)
{
    // Set up an HTTP GET request message
    if (!is_open()) {
        {
            std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
            stream_ = http_stream::create(executor_, host_, use_ssl_);
        }
        auto endpoints =
            co_await resolver_.async_resolve(host_, std::to_string(port_), net::use_awaitable);

        expires_after(true);
        co_await stream_->async_connect(endpoints);
    }

    http::request_serializer<body::any_body> serializer(req);
    while (!serializer.is_done()) {
        expires_after();
        co_await http::async_write_some(*stream_, serializer);
    }


    http::response_parser<http::empty_body> header_parser;
    header_parser.header_limit(std::numeric_limits<std::uint32_t>::max());
    header_parser.body_limit(std::numeric_limits<std::uint64_t>::max());
    while (!header_parser.is_header_done()) {
        expires_after();
        co_await http::async_read_some(*stream_, buffer_, header_parser);
    }

    http::response_parser<body::any_body> body_parser(std::move(header_parser));
    if (chunk_handler_)
        body_parser.on_chunk_body(chunk_handler_);

    if (req.method() != http::verb::head) {
        while (!body_parser.is_done()) {
            expires_after();
            co_await http::async_read_some(*stream_, buffer_, body_parser);
        }
    }
    stream_->expires_never();
    if (!body_parser.keep_alive())
        close();
    co_return body_parser.release();
}

void http_client::impl::set_chunk_handler(chunk_handler_type&& handler)
{
    if (!handler) {
        chunk_handler_ = nullptr;
        return;
    }
    chunk_handler_ = [handler = std::move(handler)](std::uint64_t remain,
                                                    std::string_view body,
                                                    boost::system::error_code& ec) -> std::size_t {
        handler(body, ec);
        return remain;
    };
}

} // namespace httplib::client
