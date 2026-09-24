#include "client_impl.h"
#include "body/write.hpp"
#include "compress/compressor.hpp"
#include "httplib/url/url.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "lazy_request_impl.hpp"
#include "redirect_util.hpp"
#include "request_impl.h"
#include "response_impl.h"
#include "util/logging.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_future.hpp>
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

    http_client::impl::impl(net::any_io_executor const& ex,
                            std::string_view host,
                            uint16_t port,
                            httplib::url::scheme s)

        : strand_(net::make_strand(ex))
        , resolver_(strand_)
        , host_(host)
        , host_value_(url::make_host_value(host, port, s))
        , port_(port)
        , scheme_(s)
        , detail::logger("httplib.client")
    {
    }

    void
    http_client::impl::apply_rate_limits() const
    {
        if (auto s = stream_.load(); s)
        {
            net::dispatch(strand_,
                          [self = shared_from_this(), s]()
                          {
                              auto to_limit = [](std::uint64_t bytes_per_second) -> std::size_t
                              {
                                  return bytes_per_second == 0 ? (std::numeric_limits<std::size_t>::max)()
                                                               : static_cast<std::size_t>(bytes_per_second);
                              };
                              s->rate_policy().read_limit(to_limit(self->download_rate_limit_.load()));
                              s->rate_policy().write_limit(to_limit(self->upload_rate_limit_.load()));
                          });
        }
    }

    void
    http_client::impl::copy_settings_from(impl const& other)
    {
        timeout_policy_.store(other.timeout_policy_.load());
        timeout_.store(other.timeout_.load());
        verify_ssl_.store(other.verify_ssl_.load());
        header_limit_.store(other.header_limit_.load());
        body_limit_.store(other.body_limit_.load());
        download_rate_limit_.store(other.download_rate_limit_.load());
        upload_rate_limit_.store(other.upload_rate_limit_.load());
        set_logger(other.get_logger());
        ca_cert_.store(other.ca_cert_.load());
    }

    void
    http_client::impl::set_download_rate_limit(std::uint64_t bytes_per_second)
    {
        download_rate_limit_ = bytes_per_second;
        apply_rate_limits();
    }

    void
    http_client::impl::set_upload_rate_limit(std::uint64_t bytes_per_second)
    {
        upload_rate_limit_ = bytes_per_second;
        apply_rate_limits();
    }

    void
    http_client::impl::set_ca_cert(std::string_view cert)
    {
        ca_cert_.store(std::make_shared<std::string const>(cert));
    }

    bool
    http_client::impl::is_open() const
    {
        auto s = stream_.load();
        return s && s->is_open();
    }

    bool
    http_client::impl::has_active_session() const
    {
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        return !read_impl_.expired() || !write_impl_.expired();
    }

    net::awaitable<void>
    http_client::impl::write_request(http::request_serializer<http::buffer_body>& serializer,
                                     http::buffer_body::value_type* body,
                                     body::source* src,
                                     boost::system::error_code& ec)
    {
        if (!serializer.is_header_done())
        {
            co_await co_connect(ec);
            if (ec)
            {
                co_return;
            }
        }
        auto write_some_fn = [&](auto& sr, auto& e) -> net::awaitable<void> { co_await async_write_some(sr, e); };
        co_await body::write_message(&serializer, body, src, write_some_fn, ec);
    }

    net::awaitable<http_client::response_result>
    http_client::impl::async_send_request_lazy(request& req)
    {
        prepare_request(req);
        http::request_serializer<http::buffer_body> serializer(get_impl(req));

        boost::system::error_code ec;
        co_await write_request(serializer, &get_impl(req).body(), get_impl(req).source(), ec);
        // 复用连接池里的死连接：header 尚未写出时透明重连并重发一次。
        if (ec && is_retryable(ec) && !serializer.is_header_done())
        {
            ec = {};
            co_await async_close();
            http::request_serializer<http::buffer_body> retry_serializer(get_impl(req));
            co_await write_request(retry_serializer, &get_impl(req).body(), get_impl(req).source(), ec);
        }
        if (ec)
        {
            co_return ec;
        }
        co_return co_await read_response_lazy(req.method());
    }
    net::awaitable<httplib::client::http_client::response_result>
    http_client::impl::read_response_lazy(http::verb method)
    {
        co_return co_await net::co_spawn(
            strand_,
            [&]() -> net::awaitable<httplib::client::http_client::response_result>
            {
                // 新响应从干净的缓冲区开始（上一连接/上一响应可能残留字节）。
                buffer_.clear();

                auto header_parser = std::make_unique<http::response_parser<http::empty_body>>();
                header_parser->skip(method == http::verb::head);
                header_parser->header_limit(header_limit_.load());
                header_parser->body_limit(body_limit_.load());

                boost::system::error_code ec;
                co_await async_read(*header_parser, true, ec);
                if (ec)
                {
                    co_return ec;
                }
                co_return client::response::impl::create(std::move(header_parser), shared_from_this());
            },
            net::use_awaitable);
    }

    net::awaitable<http_client::response_result>
    http_client::impl::async_send_request_lazy_with_redirect(request& req)
    {
        auto max_redirects = max_redirects_.load();

        if (max_redirects <= 0)
        {
            co_return co_await async_send_request_lazy(req);
        }

        for (int r = 0; r <= max_redirects; ++r)
        {
            auto result = co_await async_send_request_lazy(req);
            if (result.has_error())
            {
                co_return result;
            }
            auto& resp = result.value();
            auto s = resp.result();
            if (r < max_redirects
                && (s == http::status::moved_permanently || s == http::status::found || s == http::status::see_other
                    || s == http::status::temporary_redirect || s == http::status::permanent_redirect))
            {
                auto loc = resp[http::field::location];
                if (loc.empty())
                {
                    co_return result;
                }

                get_logger()->trace("redirect {} -> {}", req.target(), std::string_view(loc));

                // 读完并丢弃 redirect 响应的 body，保证连接可复用
                if (auto drain_result = co_await resp.read_string(); drain_result.has_error())
                {
                    co_await async_close();
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

                    if (new_host != host_ || new_port != port_ || new_ssl != (scheme_ == url::scheme::tls))
                    {
                        // CL-02: 跨 origin 重定向时移除 origin-bound 敏感头，避免认证凭据泄露到新主机
                        redirect::strip_origin_bound_headers(req.base());

                        req.target(u.encoded_target().empty() ? "/" : u.encoded_target());

                        auto new_impl = std::make_shared<impl>(strand_.get_inner_executor(),
                                                               std::move(new_host),
                                                               new_port,
                                                               new_ssl ? url::scheme::tls : url::scheme::plain);
                        new_impl->copy_settings_from(*this);
                        new_impl->max_redirects_.store(max_redirects - r - 1);

                        co_return co_await new_impl->async_send_request_lazy_with_redirect(req);
                    }

                    // 同 host/port/ssl 的完整 URL，仅取 path 作为新 target
                    target = u.encoded_target().empty() ? "/" : u.encoded_target();
                }
                else
                {
                    // 相对 Location：按 RFC 3986 针对当前 target 解析，兼容
                    // "final"、"../a/b"、"?q=1" 等形式。
                    auto base = req.target();
                    target = redirect::resolve_redirect_target(std::string_view(base.data(), base.size()), loc);
                }

                if (s == http::status::see_other
                    || ((s == http::status::moved_permanently || s == http::status::found)
                        && req.method() != http::verb::head))
                {
                    req.method(http::verb::get);
                    get_impl(req).reset_body();
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
        auto s = stream_.load();
        if (!s)
        {
            return;
        }
        switch (timeout_policy_.load())
        {
            case httplib::client::http_client::timeout_policy::overall:
            {
                if (!overall_timer_active_.load())
                {
                    s->expires_after(timeout_.load());
                    overall_timer_active_.store(true);
                }
            }
            break;
            case httplib::client::http_client::timeout_policy::step:
                s->expires_after(timeout_.load());
                break;
            case httplib::client::http_client::timeout_policy::never:
                s->expires_never();
                break;
            default:
                break;
        }
    }

    void
    http_client::impl::end_io()
    {
        switch (timeout_policy_.load())
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
        overall_timer_active_.store(false);
        if (auto s = stream_.load())
        {
            s->expires_never();
        }
    }
    void
    http_client::impl::prepare_request(request& req)
    {
        if (!req.has(http::field::host))
        {
            get_impl(req).set(http::field::host, host_value_);
        }
        // 声明了不支持的 Content-Encoding：不会被真正压缩，头却留在线上会让对端误判，
        // 这里删掉头并告警。
        auto content_encoding = get_impl(req)[http::field::content_encoding];
        if (!content_encoding.empty()
            && !compress::compressor_factory::instance().is_supported_encoding(content_encoding))
        {
            get_logger()->warn("unsupported request content-encoding '{}', remove the header",
                               std::string(content_encoding));
            req.erase(http::field::content_encoding);
        }
        if (!get_impl(req).has_content_length())
        {
            // buffer_body 不是 sized body，prepare_payload() 对空 body 也会设 Transfer-Encoding: chunked，
            // 导致服务端把空 POST 解析成 chunked body 而非空 body。空 body 显式设 Content-Length: 0。
            if (!get_impl(req).source())
            {
                get_impl(req).content_length(0);
            }
            else
            {
                get_impl(req).prepare_payload();
            }
        }
        else
        {
            // 请求带 Content-Encoding 时，write 阶段会压缩 body，set_body() 预先写入的
            // Content-Length 是明文长度，与压缩后的实际长度不一致。改用 chunked + stream encoder。
            auto content_encoding = get_impl(req)[http::field::content_encoding];
            if (!content_encoding.empty()
                && compress::compressor_factory::instance().is_transform_encoding(content_encoding))
            {
                get_impl(req).apply_encoding(content_encoding);
                get_impl(req).chunked(true);
            }
        }
    }
    net::awaitable<void>
    http_client::impl::co_connect(boost::system::error_code& ec)
    {
        finish_io();
        begin_io();

        if (!is_open())
        {
            co_await async_close();

            auto ca_cert = ca_cert_.load();
            auto stream_result = http_stream::create(strand_,
                                                     host_,
                                                     scheme_ == url::scheme::tls,
                                                     verify_ssl_.load(),
                                                     ca_cert ? std::string_view(*ca_cert) : std::string_view {});
            if (!stream_result)
            {
                ec = stream_result.error();
                co_return;
            }
            auto s = std::make_shared<http_stream>(std::move(*stream_result));
            stream_.store(s);

            apply_rate_limits();

            boost::system::error_code addr_ec;
            auto addr = net::ip::make_address(host_, addr_ec);
            if (!addr_ec)
            {
                co_await s->async_connect(tcp::endpoint(addr, port_), ec);
            }
            else
            {
                auto endpoints
                    = co_await resolver_.async_resolve(host_, std::to_string(port_), util::net_awaitable[ec]);
                if (!ec)
                {
                    co_await s->async_connect(endpoints, ec);
                }
            }
            if (ec)
            {
                get_logger()->warn("connect [{}] error {}", url::make_url_value(host_, port_, scheme_), ec.message());
                co_await async_close();
                co_return;
            }
        }
        end_io();
    }

    std::shared_ptr<lazy_request>
    http_client::impl::create_lazy_request()
    {
        std::unique_lock<std::recursive_mutex> lck(stream_mutex_);
        auto sp = write_impl_.lock();
        if (!sp)
        {
            sp = std::make_shared<lazy_request_impl>(strand_, shared_from_this());
            write_impl_ = sp;
        }
        return sp;
    }

    net::awaitable<void>
    http_client::impl::async_close()
    {
        co_await net::dispatch(strand_, net::use_awaitable);
        resolver_.cancel();
        if (auto s = stream_.exchange(nullptr); s)
        {
            boost::system::error_code ec;
            s->close(ec);
        }
    }
    std::future<void>
    http_client::impl::close()
    {
        return net::co_spawn(
            strand_,
            [self = shared_from_this()]() -> net::awaitable<void>
            {
                co_await self->async_close();
                co_return;
            },
            net::use_future);
    }

    net::awaitable<bool>
    http_client::impl::async_is_alive() const
    {
        co_await net::dispatch(strand_, net::use_awaitable);

        auto s = stream_.load();
        if (!s || !s->is_open())
        {
            co_return false;
        }
        boost::system::error_code ec;
        co_return s->is_peer_alive(ec);
    }

    http_client::http_client(net::io_context& ex, std::string_view host, uint16_t port, httplib::url::scheme s)
        : http_client(ex.get_executor(), host, port, s)
    {
    }

    http_client::http_client(net::any_io_executor const& ex,
                             std::string_view host,
                             uint16_t port,
                             httplib::url::scheme s)
        : impl_(std::make_shared<http_client::impl>(ex, host, port, s))
    {
    }

    http_client::http_client(net::io_context& ex, std::string_view url) : http_client(ex.get_executor(), url) {}

    http_client::http_client(net::any_io_executor const& ex, std::string_view url) : impl_(nullptr)
    {
        auto r = url::parse_url(url);
        if (!r)
        {
            throw std::invalid_argument(std::format("invalid url: {}", url));
        }

        auto const& u = *r;

        impl_ = std::make_shared<http_client::impl>(ex, u.host, u.effective_port(), u.transport());
    }

    http_client::~http_client() {}

    void
    http_client::set_timeout_policy(timeout_policy const& policy)
    {
        impl_->set_timeout_policy(policy);
    }

    void
    http_client::set_timeout(std::chrono::steady_clock::duration const& duration)
    {
        impl_->set_timeout(duration);
    }

    std::string_view
    http_client::host() const
    {
        return impl_->host_;
    }

    uint16_t
    http_client::port() const
    {
        return impl_->port_;
    }

    httplib::url::scheme
    http_client::scheme() const
    {
        return impl_->scheme_;
    }

    std::shared_ptr<spdlog::logger>
    http_client::logger() const
    {
        return impl_->get_logger();
    }

    void
    http_client::set_logger(std::shared_ptr<spdlog::logger> logger)
    {
        impl_->set_logger(std::move(logger));
    }

    // =============================================================================
    // core send
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_send_request(request& req, http_client::body_mode mode)
    {
        auto result = co_await impl_->async_send_request_lazy_with_redirect(req);
        if (result.has_error())
        {
            co_return result.error();
        }
        if (mode == body_mode::eager)
        {
            if (auto ec = co_await result->read_body(); ec)
            {
                co_return ec;
            }
        }
        co_return result;
    }
    net::awaitable<httplib::client::http_client::response_result>
    http_client::async_send_request(request&& req, body_mode mode /*= body_mode::eager*/)
    {
        request hold_req(std::move(req));
        co_return co_await async_send_request(hold_req, mode);
    }

    // =============================================================================
    // lazy request
    // =============================================================================

    std::shared_ptr<lazy_request>
    http_client::create_lazy_request()
    {
        return impl_->create_lazy_request();
    }

    // =============================================================================
    // HTTP method shorthands (no body)
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_get(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        request req(http::verb::get, path, params, headers);
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_head(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        request req(http::verb::head, path, params, headers);
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_post(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        request req(http::verb::post, path, params, headers);
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_put(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        request req(http::verb::put, path, params, headers);
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_patch(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        request req(http::verb::patch, path, params, headers);
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_del(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        request req(http::verb::delete_, path, params, headers);
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_options(std::string_view path, html::query_params const& params, http::fields const& headers)
    {
        request req(http::verb::options, path, params, headers);
        co_return co_await async_send_request(req);
    }

    // =============================================================================
    // HTTP method shorthands (with body)
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_post(std::string_view path,
                            std::string_view body,
                            std::string_view content_type,
                            html::query_params const& params,
                            http::fields const& headers)
    {
        auto req = request(http::verb::post, path, params, headers);
        req.set_body(body, content_type);
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_post(std::string_view path,
                            boost::json::value&& body,
                            html::query_params const& params,
                            http::fields const& headers)
    {
        auto req = request(http::verb::post, path, params, headers);
        req.set_body(std::move(body));
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_put(std::string_view path,
                           std::string_view body,
                           std::string_view content_type,
                           html::query_params const& params,
                           http::fields const& headers)
    {
        auto req = request(http::verb::put, path, params, headers);
        req.set_body(body, content_type);
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_put(std::string_view path,
                           boost::json::value&& body,
                           html::query_params const& params,
                           http::fields const& headers)
    {
        auto req = request(http::verb::put, path, params, headers);
        req.set_body(std::move(body));
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_patch(std::string_view path,
                             std::string_view body,
                             std::string_view content_type,
                             html::query_params const& params,
                             http::fields const& headers)
    {
        auto req = request(http::verb::patch, path, params, headers);
        req.set_body(body, content_type);
        co_return co_await async_send_request(req);
    }

    net::awaitable<http_client::response_result>
    http_client::async_patch(std::string_view path,
                             boost::json::value&& body,
                             html::query_params const& params,
                             http::fields const& headers)
    {
        auto req = request(http::verb::patch, path, params, headers);
        req.set_body(std::move(body));
        co_return co_await async_send_request(req);
    }

    // =============================================================================
    // Download
    // =============================================================================

    net::awaitable<http_client::response_result>
    http_client::async_download(http::verb method,
                                std::string_view path,
                                fs::path const& save_path,
                                http::fields const& headers)
    {
        auto req = request(method, path, headers);
        auto result = co_await impl_->async_send_request_lazy_with_redirect(req);
        if (result.has_error())
        {
            co_return result.error();
        }

        if (auto ec = co_await result->read_to_file(save_path); ec)
        {
            co_return ec;
        }
        co_return result;
    }

    std::future<void>
    http_client::close()
    {
        return impl_->close();
    }
    net::awaitable<void>
    http_client::async_close()
    {
        co_return co_await impl_->async_close();
    }

    bool
    http_client::is_open() const
    {
        return impl_->is_open();
    }

    bool
    http_client::has_active_session() const
    {
        return impl_->has_active_session();
    }

    net::awaitable<bool>
    http_client::async_is_alive() const
    {
        co_return co_await impl_->async_is_alive();
    }
    std::future<bool>
    http_client::is_alive() const
    {
        auto impl = impl_;
        return net::co_spawn(
            impl->strand_,
            [impl]() -> net::awaitable<bool> { co_return co_await impl->async_is_alive(); },
            net::use_future);
    }
    void
    http_client::set_max_redirects(int n)
    {
        impl_->set_max_redirects(n);
    }

    void
    http_client::set_verify_ssl(bool verify)
    {
        impl_->set_verify_ssl(verify);
    }

    void
    http_client::set_ca_cert(std::string_view cert)
    {
        impl_->set_ca_cert(cert);
    }

    void
    http_client::set_header_limit(std::uint32_t limit)
    {
        impl_->set_header_limit(limit);
    }

    void
    http_client::set_body_limit(std::uint64_t limit)
    {
        impl_->set_body_limit(limit);
    }

    void
    http_client::set_download_rate_limit(std::uint64_t bytes_per_second)
    {
        impl_->set_download_rate_limit(bytes_per_second);
    }

    void
    http_client::set_upload_rate_limit(std::uint64_t bytes_per_second)
    {
        impl_->set_upload_rate_limit(bytes_per_second);
    }
} // namespace httplib::client
