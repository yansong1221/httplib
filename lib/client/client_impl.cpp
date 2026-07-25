#include "client_impl.h"
#include "body/compressor.hpp"
#include "helper.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <fmt/format.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

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
    auto console_sink                 = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::sinks_init_list sink_list = {console_sink};
    default_logger_ = std::make_shared<spdlog::logger>("httplib.client", sink_list);
    default_logger_->set_level(spdlog::level::info);
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

std::shared_ptr<spdlog::logger> http_client::impl::logger() const
{
    if (custom_logger_)
        return custom_logger_;
    return default_logger_;
}

void http_client::impl::set_logger(std::shared_ptr<spdlog::logger> logger)
{
    custom_logger_ = std::move(logger);
}

net::awaitable<http_client::response_result>
http_client::impl::async_send_request(http_client::request& req,
                                       bool retry /*= true*/,
                                       const body_setup_fn& body_setup /*= {}*/)
{
    boost::system::error_code ec;
    try {
        http_client::response resp = co_await async_send_request_impl(req, body_setup);
        co_return resp;
    }
    catch (const boost::system::system_error& error) {
        ec = error.code();
        logger()->warn("{} {} {}: {}", host_, std::to_string(port_), error.what(), ec.message());
    }
    catch (const std::exception& e) {
        ec = boost::system::errc::make_error_code(boost::system::errc::protocol_error);
        logger()->warn("{} {}: {}", host_, std::to_string(port_), e.what());
    }
    catch (...) {
        ec = boost::system::errc::make_error_code(boost::system::errc::protocol_error);
        logger()->warn("{} {}: unknown exception", host_, std::to_string(port_));
    }
    close();

    if (ec == boost::asio::error::connection_aborted ||
        ec == boost::asio::error::connection_reset || ec == http::error::end_of_stream)
    {
        if (retry) {
            logger()->trace("retrying request...");
            co_return co_await async_send_request(req, false, body_setup);
        }
    }
    co_return ec;
}

net::awaitable<http_client::response_result>
http_client::impl::async_send_request_with_redirect(http_client::request& req,
                                                     bool retry /*= true*/,
                                                     const body_setup_fn& body_setup /*= {}*/)
{
    if (max_redirects_ <= 0)
        co_return co_await async_send_request(req, retry, body_setup);

    for (int r = 0; r <= max_redirects_; ++r) {
        boost::system::error_code ec;
        http_client::response resp;
        try {
            resp = co_await async_send_request_impl(req, body_setup);
        }
        catch (const boost::system::system_error& error) {
            ec = error.code();
            logger()->warn("{} {} {}: {}", host_, std::to_string(port_), error.what(), ec.message());
        }
        catch (const std::exception& e) {
            ec = boost::system::errc::make_error_code(boost::system::errc::protocol_error);
            logger()->warn("{} {}: {}", host_, std::to_string(port_), e.what());
        }
        catch (...) {
            ec = boost::system::errc::make_error_code(boost::system::errc::protocol_error);
            logger()->warn("{} {}: unknown exception", host_, std::to_string(port_));
        }

        if (ec) {
            close();
            if (retry &&
                (ec == boost::asio::error::connection_aborted ||
                 ec == boost::asio::error::connection_reset ||
                 ec == http::error::end_of_stream))
            {
                logger()->trace("retrying request...");
                continue;
            }
            co_return ec;
        }

        auto s = static_cast<unsigned>(resp.result());
        if (r < max_redirects_ && (s == 301 || s == 302 || s == 303 || s == 307 || s == 308)) {
            auto loc = resp[http::field::location];
            if (loc.empty())
                co_return resp;

            logger()->trace("redirect {} -> {}", req.target(), std::string(loc));

            if (s == 303 || ((s == 301 || s == 302) && req.method() != http::verb::head)) {
                req.method(http::verb::get);
                req.body() = body::empty_body::value_type{};
                req.erase(http::field::content_type);
                req.erase(http::field::content_length);
                req.prepare_payload();
            }

            req.target(std::string(loc));
            req.set(http::field::host, host_);
            continue;
        }

        co_return resp;
    }

    co_return boost::system::errc::make_error_code(
        boost::system::errc::too_many_symbolic_link_levels);
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
http_client::impl::async_send_request_impl(http_client::request& req,
                                            const body_setup_fn& body_setup /*= {}*/)
{
    // Set up an HTTP GET request message
    if (!is_open()) {
        {
            std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
            stream_ = std::make_unique<http_stream>(executor_, host_, use_ssl_);
        }
        logger()->trace("{} connecting to {}:{}", req.method_string(), host_, port_);

        boost::system::error_code ec;
        auto addr = net::ip::make_address(host_, ec);
        if (!ec) {
            expires_after(true);
            co_await stream_->async_connect(tcp::endpoint(addr, port_));
        }
        else {
            auto endpoints =
                co_await resolver_.async_resolve(host_, std::to_string(port_), net::use_awaitable);
            expires_after(true);
            co_await stream_->async_connect(endpoints);
        }
    }

    http::request_serializer<body::any_body> serializer(req);
    while (!serializer.is_done()) {
        expires_after();
        co_await http::async_write_some(*stream_, serializer);
    }


    if (req.method() == http::verb::head) {
        http::response_parser<http::empty_body> parser;
        parser.skip(true);
        parser.header_limit(std::numeric_limits<std::uint32_t>::max());
        parser.body_limit(std::numeric_limits<std::uint64_t>::max());

        while (!parser.is_done()) {
            expires_after();
            co_await http::async_read_some(*stream_, buffer_, parser);
        }

        stream_->expires_never();
        if (!parser.keep_alive())
            close();

        auto header_response = parser.release();
        http_client::response response;
        response.version(header_response.version());
        response.result(header_response.result());
        response.keep_alive(header_response.keep_alive());
        for (const auto& field : header_response)
            response.set(field.name_string(), field.value());
        co_return response;
    }

    http::response_parser<body::any_body> parser;
    parser.header_limit(std::numeric_limits<std::uint32_t>::max());
    parser.body_limit(std::numeric_limits<std::uint64_t>::max());

    if (body_setup)
        body_setup(parser.get().body());

    if (chunk_handler_)
        parser.on_chunk_body(chunk_handler_);

    while (!parser.is_header_done()) {
        expires_after();
        co_await http::async_read_some(*stream_, buffer_, parser);
    }

    const auto status = parser.get().result();
    if (status == http::status::no_content || status == http::status::not_modified) {
        stream_->expires_never();
        if (!parser.keep_alive())
            close();

        const auto& header_response = parser.get();
        http_client::response response;
        response.version(header_response.version());
        response.result(header_response.result());
        response.keep_alive(header_response.keep_alive());
        for (const auto& field : header_response)
            response.set(field.name_string(), field.value());
        co_return response;
    }

    while (!parser.is_done()) {
        expires_after();
        co_await http::async_read_some(*stream_, buffer_, parser);
    }

    stream_->expires_never();
    if (!parser.keep_alive())
        close();
    auto resp = parser.release();
    logger()->trace("{} {} -> {} {}",
                    req.method_string(),
                    req.target(),
                    resp.result_int(),
                    resp.reason());
    co_return resp;
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
        return body.size();
    };
}

net::awaitable<http_client::response_result>
http_client::impl::async_download(http_client::request& req, const fs::path& save_path)
{
    auto setup = [save_path](body::any_body::value_type& body) {
        body       = body::file_body::value_type {};
        auto& fb   = std::get<body::file_body::value_type>(body);
        fb.open(save_path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!fb.is_open())
            throw boost::system::system_error(
                boost::system::errc::make_error_code(boost::system::errc::permission_denied));
    };
    auto result = co_await async_send_request(req, true, setup);

    if (!result.has_value() ||
        result->result() == http::status::no_content ||
        result->result() == http::status::not_modified) {
        std::error_code ec;
        fs::remove(save_path, ec);
    }
    co_return result;
}

} // namespace httplib::client
