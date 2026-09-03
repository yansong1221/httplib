#include "client_impl.h"
#include "compress/compressor.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "lazy_request_impl.hpp"
#include "request_impl.h"
#include "response_impl.h"
#include "util/logging.hpp"
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
#include <boost/url.hpp>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <spdlog/spdlog.h>

namespace httplib::client
{
    http_client::impl::impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl)

        : executor_(ex)
        , strand_(net::make_strand(ex))
        , resolver_(ex)
        , host_(host)
        , host_value_(util::make_host_value(host, port, ssl))
        , port_(port)
        , use_ssl_(ssl)
    {
        default_logger_ = httplib::detail::make_console_logger("httplib.client");

        buffer_.reserve(io_buffer_size);
    }

    void
    http_client::impl::close()
    {
        finish_io();

        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        resolver_.cancel();
        if (stream_)
        {
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

    bool
    http_client::impl::has_active_session() const
    {
        return !read_impl_.expired() || !write_impl_.expired();
    }

    bool
    http_client::impl::is_alive() const
    {
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        if (!stream_ || !stream_->is_open())
        {
            return false;
        }
        boost::system::error_code ec;
        return stream_->is_peer_alive(ec);
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
    http_client::impl::async_send_request_lazy(http_client::request& req)
    {
        prepare_request(req);
        http::request_serializer<body::any_body> serializer(get_impl(req));
        if (auto ec = co_await async_write(serializer, false); ec)
        {
            co_return ec;
        }

        auto header_parser = std::make_unique<http::response_parser<http::empty_body>>();
        header_parser->skip(req.method() == http::verb::head);
        header_parser->header_limit(header_limit_);
        header_parser->body_limit(body_limit_);

        auto ec = co_await async_read(*header_parser, true);
        if (ec)
        {
            co_return ec;
        }

        co_return client::response::impl::make_lazy(std::move(header_parser), shared_from_this());
    }

    net::awaitable<http_client::response_result>
    http_client::impl::async_send_request_lazy_with_redirect(http_client::request& req)
    {
        auto self = shared_from_this();

        if (max_redirects_ <= 0)
        {
            co_return co_await async_send_request_lazy(req);
        }

        for (int r = 0; r <= max_redirects_; ++r)
        {
            auto result = co_await async_send_request_lazy(req);
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

                // 读完并丢弃 redirect 响应的 body，保证连接可复用
                if (auto drain_result = co_await resp.read_string(); drain_result.has_error())
                {
                    close();
                }

                // Full URL (cross-domain) create new impl
                std::string target;
                if (loc.starts_with("http://") || loc.starts_with("https://"))
                {
                    auto u = boost::urls::url(loc);
                    auto new_host = u.host();
                    auto new_port
                        = u.port_number() ? u.port_number() : (u.scheme_id() == boost::urls::scheme::https ? 443 : 80);
                    auto new_ssl = u.scheme_id() == boost::urls::scheme::https;

                    if (new_host != host_ || new_port != port_ || new_ssl != use_ssl_)
                    {
                        req.target(u.encoded_target().empty() ? "/" : u.encoded_target());

                        auto new_impl = std::make_shared<impl>(executor_, std::move(new_host), new_port, new_ssl);
                        new_impl->timeout_policy_ = timeout_policy_;
                        new_impl->timeout_ = timeout_;
                        new_impl->verify_ssl_ = verify_ssl_;
                        new_impl->set_logger(logger());
                        new_impl->max_redirects_ = max_redirects_ - r - 1;
                        new_impl->header_limit_ = header_limit_;
                        new_impl->body_limit_ = body_limit_;

                        co_return co_await new_impl->async_send_request_lazy_with_redirect(req);
                    }

                    // 同 host/port/ssl 的完整 URL，仅取 path 作为新 target
                    target = u.encoded_target().empty() ? "/" : u.encoded_target();
                }
                else
                {
                    target = loc;
                }

                if (s == http::status::see_other
                    || ((s == http::status::moved_permanently || s == http::status::found)
                        && req.method() != http::verb::head))
                {
                    req.method(http::verb::get);
                    get_impl(req).body() = body::empty_body::value_type {};
                    req.erase(http::field::content_type);
                    req.erase(http::field::content_length);
                    get_impl(req).prepare_payload();
                }

                req.target(std::move(target));
                continue;
            }

            co_return result;
        }

        co_return boost::system::errc::make_error_code(boost::system::errc::too_many_symbolic_link_levels);
    }

    void
    http_client::impl::begin_io()
    {
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        if (stream_)
        {
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
                if (!overall_timer_active_)
                {
                    stream_->expires_after(timeout_);
                    overall_timer_active_ = true;
                }
            }
        }
    }

    void
    http_client::impl::end_io()
    {
        switch (timeout_policy_)
        {
            case http_client::timeout_policy::step:
            case http_client::timeout_policy::never:
                finish_io();
                break;
            default:
                break;
        }
    }

    void
    http_client::impl::finish_io()
    {
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        overall_timer_active_ = false;
        if (stream_)
        {
            stream_->expires_never();
        }
    }
    void
    http_client::impl::prepare_request(http_client::request& req)
    {
        if (!req.has(http::field::host))
        {
            get_impl(req).set(http::field::host, host_value_);
        }
        if (!get_impl(req).has_content_length())
        {
            // any_body 不是 sized body，prepare_payload() 对空 body 也会设 Transfer-Encoding: chunked，
            // 导致服务端把空 POST 解析成 chunked body 而非 empty_body。空 body 显式设 Content-Length: 0。
            if (std::holds_alternative<body::empty_body::value_type>(get_impl(req).body()))
            {
                get_impl(req).content_length(0);
            }
            else
            {
                get_impl(req).prepare_payload();
            }
        }
    }
    net::awaitable<boost::system::error_code>
    http_client::impl::co_connect()
    {
        finish_io();
        begin_io();
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        if (!is_open())
        {
            close();
            // 用 strand 作为 executor 创建流：连接上的读写由此串行化，
            // 避免并发流式读取（read_some_raw / read_some_decompressed）交错。
            auto stream_result = http_stream::create_stream(strand_, host_, use_ssl_, verify_ssl_, ca_cert_);
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
                logger()->warn("connect [{}] error {}", util::make_url_value(host_, port_, use_ssl_), ec.message());
                close();
                co_return ec;
            }
        }
        end_io();
        co_return boost::system::error_code {};
    }

    std::shared_ptr<lazy_request>
    http_client::impl::create_lazy_request()
    {
        auto sp = write_impl_.lock();
        if (!sp)
        {
            sp = std::make_shared<lazy_request_impl>(shared_from_this());
            write_impl_ = sp;
        }
        return sp;
    }

} // namespace httplib::client
