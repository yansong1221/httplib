#include "client_impl.h"
#include "compress/compressor.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "streaming/chunk_reader_impl.hpp"
#include "streaming/chunk_writer_impl.hpp"
#include "streaming/ndjson_reader_impl.hpp"
#include "streaming/sse_reader_impl.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace httplib::client
{
    namespace detail
    {

        static std::string
        make_host_value(std::string_view host, uint16_t port, bool ssl)
        {
            if ((ssl && port != 443) || (!ssl && port != 80))
            {
                return fmt::format("{}:{}", host, port);
            }
            return std::string(host);
        }

        static http::field const hop_by_hop_fields[] = {
            http::field::connection,
            http::field::keep_alive,
            http::field::transfer_encoding,
            http::field::te,
            http::field::trailer,
            http::field::proxy_authorization,
            http::field::proxy_authenticate,
            http::field::upgrade,
        };

        static bool
        is_hop_by_hop(http::field f)
        {
            for (auto hf : hop_by_hop_fields)
            {
                if (f == hf)
                {
                    return true;
                }
            }
            return false;
        }

    } // namespace detail

    class http_client::impl::relay_impl final : public relay_session
    {
      public:
        explicit relay_impl(http_client::impl& parent) : parent_(parent) {}

        net::awaitable<boost::system::error_code>
        write_header(http::verb method, std::string_view target, http::fields const& headers) override
        {
            req_msg_ = std::make_unique<http::request<http::buffer_body>>(method, target, 11);
            req_sr_ = std::make_unique<http::request_serializer<http::buffer_body>>(*req_msg_);
            req_msg_->set(http::field::host, parent_.host_value_);
            req_msg_->set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
            req_msg_->keep_alive(true);

            for (auto const& f : headers)
            {
                if (!detail::is_hop_by_hop(f.name()))
                {
                    req_msg_->set(f.name_string(), f.value());
                }
            }

            co_return co_await parent_.async_write(*req_sr_, true);
        }

      private:
        net::awaitable<boost::system::error_code>
        write_body(net::const_buffer const& data, bool more) override
        {
            auto& body = req_msg_->body();
            body.data = (void*)data.data();
            body.size = data.size();
            body.more = more;

            auto ec = co_await parent_.async_write(*req_sr_, false, false);
            if (ec == http::error::need_buffer)
            {
                ec = {};
            }
            co_return ec;
        }

        net::awaitable<boost::system::error_code>
        read_header() override
        {
            resp_parser_ = std::make_unique<http::response_parser<http::buffer_body>>();
            resp_parser_->body_limit((std::numeric_limits<std::uint64_t>::max)());
            resp_parser_->header_limit((std::numeric_limits<std::uint32_t>::max)());

            parent_.buffer_.clear();
            co_return co_await parent_.async_read(*resp_parser_, true);
        }

        http::status
        result() const override
        {
            return resp_parser_->get().result();
        }

        http::fields const&
        headers() const override
        {
            return resp_parser_->get();
        }

        net::awaitable<boost::system::result<std::size_t>>
        read_body(net::mutable_buffer const& buffer) override
        {
            if (!resp_parser_->is_header_done())
            {
                co_return 0;
            }

            for (;;)
            {
                auto& body = resp_parser_->get().body();
                body.data = buffer.data();
                body.size = buffer.size();

                auto ec = co_await parent_.async_read(*resp_parser_, false);
                if (ec == http::error::need_buffer)
                {
                    ec = {};
                }
                if (ec)
                {
                    co_return ec;
                }

                auto consumed = buffer.size() - body.size;
                if (consumed > 0)
                {
                    co_return consumed;
                }

                if (resp_parser_->is_done())
                {
                    parent_.finish_io();
                    if (!resp_parser_->keep_alive())
                    {
                        parent_.close();
                    }
                    co_return 0;
                }
            }
        }

        http_client::impl& parent_;
        std::unique_ptr<http::request<http::buffer_body>> req_msg_;
        std::unique_ptr<http::request_serializer<http::buffer_body>> req_sr_;
        std::unique_ptr<http::response_parser<http::buffer_body>> resp_parser_;
    };

    http_client::impl::impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl)

        : executor_(ex)
        , resolver_(ex)
        , host_(host)
        , host_value_(detail::make_host_value(host, port, ssl))
        , port_(port)
        , use_ssl_(ssl)
        , relay_(std::make_unique<relay_impl>(*this))
    {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        spdlog::sinks_init_list sink_list = { console_sink };
        default_logger_ = std::make_shared<spdlog::logger>("httplib.client", sink_list);
        default_logger_->set_level(spdlog::level::info);
    }
    http_client::impl::~impl() { close(); }

    http_client::request
    http_client::impl::make_http_request(http::verb method, std::string_view target, http::fields const& headers) const
    {
        http_client::request req(method, target, 11);
        req.set(http::field::host, host_value_);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::accept, "*/*");

        if (!chunked_read_handler_)
        {
            auto const& encoding = compress::compressor_factory::instance().supported_encoding();
            if (!encoding.empty())
            {
                req.set(http::field::accept_encoding, boost::join(encoding, ","));
            }
        }

        for (auto const& field : headers)
        {
            req.set(field.name_string(), field.value());
        }
        req.keep_alive(true);
        return std::move(req);
    }

    void
    http_client::impl::close()
    {
        resolver_.cancel();

        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        if (stream_)
        {
            stream_->expires_never();
            stream_->close();
        }
        buffer_.clear();
    }

    bool
    http_client::impl::is_open() const
    {
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        return stream_ && stream_->is_open();
    }

    std::shared_ptr<spdlog::logger>
    http_client::impl::logger() const
    {
        if (custom_logger_)
        {
            return custom_logger_;
        }
        return default_logger_;
    }

    void
    http_client::impl::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        custom_logger_ = std::move(logger);
    }

    net::awaitable<http_client::response_result>
    http_client::impl::async_send_request(http_client::request& req,
                                          body_setup_fn const& body_setup,
                                          chunked_write_handler_type const& chunked_write_handler) noexcept
    {
        http::request_serializer<body::any_body> serializer(req);
        auto ec = co_await async_write(serializer, chunked_write_handler != nullptr);
        if (ec)
        {
            co_return ec;
        }

        if (chunked_write_handler)
        {
            streaming::chunk_writer_impl writer(*stream_, timeout_);
            co_await chunked_write_handler(writer);
            co_await writer.close();
        }

        buffer_.clear();
        co_return co_await co_read_response(body_setup, req.method() == http::verb::head);
    }

    net::awaitable<http_client::response_result>
    http_client::impl::async_send_request_with_redirect(http_client::request& req,
                                                        body_setup_fn const& body_setup,
                                                        chunked_write_handler_type body_write)
    {
        if (max_redirects_ <= 0)
        {
            co_return co_await async_send_request(req, body_setup, body_write);
        }

        for (int r = 0; r <= max_redirects_; ++r)
        {
            auto result = co_await async_send_request(req, body_setup, body_write);
            if (result.has_error())
            {
                co_return result;
            }
            auto& resp = result.value();
            auto s = resp.result();
            if (r < max_redirects_
                && (s == http::status::moved_permanently || s == http::status::found || s == http::status::see_other
                    || s == http::status::temporary_redirect || s == http::status::permanent_redirect))
            {
                auto loc = resp[http::field::location];
                if (loc.empty())
                {
                    co_return result;
                }

                logger()->trace("redirect {} -> {}", req.target(), std::string_view(loc));

                // Full URL (cross-domain) create new impl
                if (loc.starts_with("http://") || loc.starts_with("https://"))
                {
                    auto scheme_end = loc.find("://");
                    auto path_start = loc.find('/', scheme_end + 3);
                    auto host_port = path_start == std::string_view::npos
                                         ? loc.substr(scheme_end + 3)
                                         : loc.substr(scheme_end + 3, path_start - scheme_end - 3);

                    auto colon = host_port.find(':');
                    auto new_host = colon == std::string_view::npos ? std::string(host_port)
                                                                    : std::string(host_port.substr(0, colon));
                    auto new_ssl = loc.starts_with("https://");
                    uint16_t new_port = new_ssl ? 443 : 80;
                    if (colon != std::string_view::npos)
                    {
                        new_port = static_cast<uint16_t>(std::stoul(std::string(host_port.substr(colon + 1))));
                    }

                    auto new_target
                        = path_start == std::string_view::npos ? std::string("/") : std::string(loc.substr(path_start));

                    req.target(std::move(new_target));
                    req.set(http::field::host, new_host);

                    auto new_impl = std::make_unique<impl>(executor_, new_host, new_port, new_ssl);
                    new_impl->timeout_policy_ = timeout_policy_;
                    new_impl->timeout_ = timeout_;
                    new_impl->set_logger(logger());
                    new_impl->max_redirects_ = max_redirects_ - r - 1;

                    co_return co_await new_impl->async_send_request(req, body_setup, body_write);
                }

                if (s == http::status::see_other
                    || ((s == http::status::moved_permanently || s == http::status::found)
                        && req.method() != http::verb::head))
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

        co_return boost::system::errc::make_error_code(boost::system::errc::too_many_symbolic_link_levels);
    }
    void
    http_client::impl::begin_io(bool first)
    {
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        if (!stream_)
        {
            return;
        }

        if (timeout_policy_ == timeout_policy::step)
        {
            stream_->expires_after(timeout_);
        }
        else if (timeout_policy_ == timeout_policy::never)
        {
            stream_->expires_never();
        }
        else if (timeout_policy_ == timeout_policy::overall)
        {
            if (!first)
            {
                return;
            }
            stream_->expires_after(timeout_);
        }
    }

    void
    http_client::impl::end_io()
    {
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        if (!stream_)
        {
            return;
        }

        switch (timeout_policy_)
        {
            case http_client::timeout_policy::step:
            case http_client::timeout_policy::never:
                stream_->expires_never();
                break;
            default:
                break;
        }
    }

    void
    http_client::impl::finish_io()
    {
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        if (!stream_)
        {
            return;
        }

        stream_->expires_never();
    }

    net::awaitable<boost::system::error_code>
    http_client::impl::co_connect()
    {
        begin_io(true);
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        if (!is_open())
        {
            close();
            auto stream_result = http_stream::create_stream(executor_, host_, use_ssl_);
            if (!stream_result)
            {
                co_return stream_result.error();
            }
            stream_ = std::make_unique<http_stream>(std::move(*stream_result));
            lck.unlock();

            boost::system::error_code ec;
            auto addr = net::ip::make_address(host_, ec);
            if (!ec)
            {
                ec = co_await stream_->async_connect(tcp::endpoint(addr, port_));
            }
            else
            {
                auto endpoints
                    = co_await resolver_.async_resolve(host_, std::to_string(port_), util::net_awaitable[ec]);
                if (!ec)
                {
                    ec = co_await stream_->async_connect(endpoints);
                }
            }
            if (ec)
            {
                close();
                co_return ec;
            }
        }
        end_io();
        co_return boost::system::error_code {};
    }

    net::awaitable<http_client::response_result>
    http_client::impl::co_read_response(body_setup_fn const& body_setup /*= {}*/, bool is_head /*= false*/)
    {
        boost::system::error_code ec;
        http::response_parser<body::any_body> parser;
        parser.skip(is_head);
        parser.eager(false);
        parser.header_limit(std::numeric_limits<std::uint32_t>::max());
        parser.body_limit(std::numeric_limits<std::uint64_t>::max());

        while (!parser.is_header_done())
        {
            begin_io();
            co_await http::async_read_some(*stream_, buffer_, parser, util::net_awaitable[ec]);
            if (ec)
            {
                close();
                co_return ec;
            }
            end_io();
        }

        if (!parser.is_done())
        {
            auto ct = parser.get()[http::field::content_type];
            auto semi = ct.find(';');
            auto mime = semi == std::string_view::npos ? ct : ct.substr(0, semi);
            bool is_sse = beast::iequals(mime, "text/event-stream");
            bool is_ndjson = beast::iequals(mime, "application/x-ndjson");

            if (parser.chunked() && sse_read_handler_ && is_sse)
            {
                streaming::chunk_reader_impl<false> reader_impl(*stream_, buffer_, parser, timeout_);

                streaming::sse_reader_impl sse(reader_impl);
                co_await sse_read_handler_(sse);
            }
            else if (parser.chunked() && ndjson_read_handler_ && is_ndjson)
            {
                streaming::chunk_reader_impl<false> reader_impl(*stream_, buffer_, parser, timeout_);

                streaming::ndjson_reader_impl ndjson(reader_impl);
                co_await ndjson_read_handler_(ndjson);
            }
            else if (parser.chunked() && chunked_read_handler_)
            {
                streaming::chunk_reader_impl<false> reader_impl(*stream_, buffer_, parser, timeout_);

                co_await chunked_read_handler_(reader_impl, parser.get());
            }
            else
            {
                if (body_setup)
                {
                    body_setup(parser.get());
                }

                while (!parser.is_done())
                {
                    begin_io();
                    co_await http::async_read_some(*stream_, buffer_, parser, util::net_awaitable[ec]);
                    if (ec)
                    {
                        close();
                        co_return ec;
                    }
                    end_io();
                }
            }
        }

        finish_io();
        if (!parser.keep_alive())
        {
            close();
        }
        co_return parser.release();
    }

    void
    http_client::impl::set_chunked_read_handler(chunked_read_handler_type&& handler)
    {
        chunked_read_handler_ = std::move(handler);
    }

    void
    http_client::impl::set_sse_read_handler(sse_read_handler_type&& handler)
    {
        sse_read_handler_ = std::move(handler);
    }

    void
    http_client::impl::set_ndjson_read_handler(ndjson_read_handler_type&& handler)
    {
        ndjson_read_handler_ = std::move(handler);
    }

    net::awaitable<http_client::response_result>
    http_client::impl::async_download(http_client::request& req, fs::path const& save_path)
    {
        auto fb_ptr = std::make_shared<body::file_body::value_type>();
        fb_ptr->open(save_path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!fb_ptr->is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
        }

        auto setup = [fb_ptr](response& resp) { resp.body() = std::move(*fb_ptr); };
        auto result = co_await async_send_request_with_redirect(req, setup, nullptr);

        if (!result.has_value() || result->result() == http::status::no_content
            || result->result() == http::status::not_modified)
        {
            std::error_code ec;
            fs::remove(save_path, ec);
        }
        co_return result;
    }

    relay_session&
    http_client::impl::session()
    {
        return *relay_;
    }

} // namespace httplib::client
