#include "client_impl.h"
#include "body/compressor.hpp"
#include "helper.hpp"
#include "httplib/util/use_awaitable.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/write.hpp>
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
                                      const body_setup_fn& body_setup,
                                      const chunk_body_generator& body_write,
                                      bool retry) noexcept
{
    boost::system::error_code ec;
    try {
        http_client::response resp = co_await async_send_request_impl(req, body_setup, body_write);
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
            co_return co_await async_send_request(req, body_setup, body_write, false);
        }
    }
    co_return ec;
}

net::awaitable<http_client::response_result>
http_client::impl::async_send_request_with_redirect(http_client::request& req,
                                                    const body_setup_fn& body_setup,
                                                    const chunk_body_generator& body_write)
{
    if (max_redirects_ <= 0)
        co_return co_await async_send_request(req, body_setup, body_write, true);

    for (int r = 0; r <= max_redirects_; ++r) {
        auto result = co_await async_send_request(req, body_setup, body_write, true);
        if (result.has_error()) {
            co_return result;
        }
        auto& resp = result.value();
        auto s     = resp.result();
        if (r < max_redirects_ &&
            (s == http::status::moved_permanently || s == http::status::found ||
             s == http::status::see_other || s == http::status::temporary_redirect ||
             s == http::status::permanent_redirect))
        {
            auto loc = resp[http::field::location];
            if (loc.empty())
                co_return result;

            logger()->trace("redirect {} -> {}", req.target(), std::string_view(loc));

            // Full URL (cross-domain) → create new impl
            if (loc.starts_with("http://") || loc.starts_with("https://")) {
                auto scheme_end = loc.find("://");
                auto path_start = loc.find('/', scheme_end + 3);
                auto host_port  = path_start == std::string_view::npos
                                    ? loc.substr(scheme_end + 3)
                                    : loc.substr(scheme_end + 3, path_start - scheme_end - 3);

                auto colon     = host_port.find(':');
                auto new_host  = colon == std::string_view::npos
                                     ? std::string(host_port)
                                     : std::string(host_port.substr(0, colon));
                auto new_ssl   = loc.starts_with("https://");
                uint16_t new_port = new_ssl ? 443 : 80;
                if (colon != std::string_view::npos)
                    new_port =
                        static_cast<uint16_t>(std::stoul(std::string(host_port.substr(colon + 1))));

                auto new_target = path_start == std::string_view::npos
                                    ? std::string("/")
                                    : std::string(loc.substr(path_start));

                req.target(std::move(new_target));
                req.set(http::field::host, new_host);

                auto new_impl = std::make_unique<impl>(executor_, new_host, new_port, new_ssl);
                new_impl->timeout_policy_ = timeout_policy_;
                new_impl->timeout_        = timeout_;
                new_impl->set_logger(logger());
                new_impl->max_redirects_  = max_redirects_ - r - 1;

                co_return co_await new_impl->async_send_request(req, body_setup, body_write, true);
            }

            if (s == http::status::see_other ||
                ((s == http::status::moved_permanently || s == http::status::found) &&
                 req.method() != http::verb::head))
            {
                req.method(http::verb::get);
                req.body() = body::empty_body::value_type {};
                req.erase(http::field::content_type);
                req.erase(http::field::content_length);
                req.prepare_payload();
            }

            req.target(std::string(loc));
            req.set(http::field::host, host_);
            continue;
        }

        co_return result;
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

net::awaitable<void> http_client::impl::co_connect()
{
    std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
    if (!is_open()) {
        stream_ = std::make_unique<http_stream>(executor_, host_, use_ssl_);
        lck.unlock();

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
    expires_after();
}

net::awaitable<void> http_client::impl::co_write_request(http::request<body::any_body>& req,
                                                         bool headers_only)
{
    http::request_serializer<body::any_body> serializer(req);
    if (headers_only)
        serializer.split(true);

    while (headers_only ? !serializer.is_header_done() : !serializer.is_done()) {
        expires_after();
        co_await http::async_write_some(*stream_, serializer, boost::asio::use_awaitable);
    }
    expires_after();
}

net::awaitable<http_client::response> http_client::impl::co_read_response(
    const body_setup_fn& body_setup /*= {}*/, bool is_head /*= false*/)
{
    if (is_head) {
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

        auto hr = parser.release();
        http_client::response response;
        response.version(hr.version());
        response.result(hr.result());
        response.keep_alive(hr.keep_alive());
        for (const auto& field : hr)
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

        const auto& hr = parser.get();
        http_client::response response;
        response.version(hr.version());
        response.result(hr.result());
        response.keep_alive(hr.keep_alive());
        for (const auto& field : hr)
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
    co_return parser.release();
}

net::awaitable<http_client::response>
http_client::impl::async_send_request_impl(http_client::request& req,
                                           const body_setup_fn& body_setup,
                                           const chunk_body_generator& chunk_body_write)
{
    co_await co_connect();
    co_await co_write_request(req, chunk_body_write != nullptr);

    if (chunk_body_write) {
        for (boost::system::error_code ec;;) {
            expires_after();
            buffer_.consume(buffer_.size());
            bool has_more = co_await chunk_body_write(buffer_, ec);
            if (ec) {
                throw boost::system::system_error(ec);
            }
            if (buffer_.size() != 0) {
                http::chunk_body chunk_b(buffer_.data());
                expires_after();
                co_await net::async_write(*stream_, chunk_b, boost::asio::use_awaitable);
            }
            if (!has_more) {
                http::chunk_last last_chunk;
                expires_after();
                co_await net::async_write(*stream_, last_chunk, boost::asio::use_awaitable);
                break;
            }
        }
        buffer_.consume(buffer_.size());
    }

    co_return co_await co_read_response(body_setup, req.method() == http::verb::head);
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
        body     = body::file_body::value_type {};
        auto& fb = std::get<body::file_body::value_type>(body);
        fb.open(save_path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!fb.is_open())
            throw boost::system::system_error(
                boost::system::errc::make_error_code(boost::system::errc::permission_denied));
    };
    auto result = co_await async_send_request_with_redirect(req, setup, nullptr);

    if (!result.has_value() || result->result() == http::status::no_content ||
        result->result() == http::status::not_modified)
    {
        std::error_code ec;
        fs::remove(save_path, ec);
    }
    co_return result;
}

} // namespace httplib::client
