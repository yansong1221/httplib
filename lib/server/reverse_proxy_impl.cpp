#include "reverse_proxy_impl.h"
#include "httplib/util/misc.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/url.hpp>
#include <spdlog/spdlog.h>
#include <string_view>

namespace httplib::server::detail
{
    namespace
    {
        /// Maps an upstream transport error to a client-facing status code.
        http::status
        upstream_error_to_status(boost::system::error_code ec)
        {
            if (ec == boost::asio::error::timed_out || ec == boost::beast::error::timeout)
            {
                return http::status::gateway_timeout;
            }
            return http::status::bad_gateway;
        }

        /// Strips trailing '/' and '*' from a proxy route prefix.
        std::string
        strip_proxy_prefix(std::string_view route)
        {
            std::string result(route);
            while (!result.empty() && (result.back() == '/' || result.back() == '*'))
            {
                result.pop_back();
            }
            return result;
        }

        /// Joins a client target path with the proxy prefix onto an upstream base path.
        std::string
        make_upstream_path(std::string_view client_target,
                           std::string_view proxy_prefix,
                           std::string_view upstream_base)
        {
            if (!client_target.starts_with(proxy_prefix))
            {
                return {};
            }

            auto tail = client_target.substr(proxy_prefix.size());

            if (tail.empty())
            {
                return upstream_base.empty() ? "/" : std::string(upstream_base);
            }

            std::string result(upstream_base);

            if (result.size() > 1 && result.ends_with('/'))
            {
                result.pop_back();
            }

            if (tail.front() != '/' && tail.front() != '?')
            {
                result += '/';
            }
            else if (result.ends_with('/') && tail.front() == '/')
            {
                tail.remove_prefix(1);
            }

            result += tail;

            return result;
        }
    } // namespace

    reverse_proxy_context::reverse_proxy_context(std::shared_ptr<client::http_client_pool> pool,
                                                 std::string prefix,
                                                 http_server::proxy_resolver resolver,
                                                 http_server::proxy_interceptor_factory factory,
                                                 std::shared_ptr<spdlog::logger> logger)
        : pool_(std::move(pool))
        , prefix_(std::move(prefix))
        , resolver_(std::move(resolver))
        , factory_(std::move(factory))
        , logger_(std::move(logger))
    {
    }

    net::awaitable<void>
    reverse_proxy_context::run(request& req, response& resp)
    {
        if (!co_await prepare_upstream(req, resp))
        {
            co_return;
        }

        build_upstream_headers(req, resp);

        if (!co_await forward_request(req, resp))
        {
            co_return;
        }

        if (!co_await read_upstream_response(req, resp))
        {
            co_return;
        }

        co_await relay_response(resp);
    }

    net::awaitable<bool>
    reverse_proxy_context::prepare_upstream(request& req, response& resp)
    {
        interceptor_ = factory_ ? factory_(req) : nullptr;

        target_ = co_await resolver_(req);
        if (!target_)
        {
            logger_->trace("[proxy] resolver returned null target");
            resp.set_error_content(http::status::bad_gateway);
            co_return false;
        }
        auto const& url = target_->url();
        logger_->debug("[proxy] {} {} -> {}", req.method_string(), req.target(), url);

        auto r = boost::urls::parse_uri(url);
        if (!r)
        {
            logger_->trace("[proxy] invalid upstream url: {}", url);
            resp.set_error_content(http::status::bad_gateway);
            co_return false;
        }

        auto const& u = *r;
        upstream_host_ = std::string(u.host());
        port_ = (u.scheme_id() == boost::urls::scheme::https ? 443 : 80);
        port_ = u.has_port() ? u.port_number() : port_;
        ssl_ = u.scheme_id() == boost::urls::scheme::https;
        upstream_prefix_ = std::string(u.encoded_path());
        upstream_scheme_ = std::string(u.scheme());
        upstream_target_ = make_upstream_path(req.target(), prefix_, upstream_prefix_);
        upstream_url_ = util::make_url_value(u.host(), port_, ssl_, upstream_target_);

        co_return true;
    }

    void
    reverse_proxy_context::build_upstream_headers(request& req, response& resp)
    {
        (void)resp;
        http::fields upstream_headers(req.base());

        // Strip and rewrite Cookie Domain/Path before forwarding upstream
        if (auto cookie = req[http::field::cookie]; !cookie.empty())
        {
            std::vector<std::string> rewritten;
            for (auto item : util::split(cookie, ";"))
            {
                auto parts = util::split(item, "=");
                if (parts.empty())
                {
                    continue;
                }
                auto key = parts.front();
                if (boost::iequals(key, "Domain") || boost::iequals(key, "Path"))
                {
                    continue;
                }
                rewritten.emplace_back(item);
            }
            using namespace std::string_view_literals;
            rewritten.emplace_back(std::format("Domain={}", util::make_host_value(upstream_host_, port_, ssl_)));
            rewritten.emplace_back(std::format("Path={}", upstream_prefix_.empty() ? "/"sv : upstream_prefix_));
            upstream_headers.set(http::field::cookie, boost::join(rewritten, ";"));
        }

        auto client_ip = req.remote_endpoint().address().to_string();
        if (auto xff = req["X-Forwarded-For"]; !xff.empty())
        {
            client_ip = std::format("{},{}", xff, client_ip);
        }
        upstream_headers.set("X-Forwarded-For", client_ip);
        upstream_headers.set("X-Forwarded-Proto", ssl_ ? "https" : "http");
        upstream_headers.set("X-Forwarded-Host", req["Host"]);

        // Rewrite Referer to upstream
        if (auto ref = req[http::field::referer]; !ref.empty())
        {
            auto r = boost::urls::parse_uri(ref);
            if (r)
            {
                auto ref_path = std::string(r->encoded_path());
                if (ref_path.starts_with(prefix_))
                {
                    ref_path = ref_path.substr(prefix_.size());
                }
                if (!upstream_prefix_.empty())
                {
                    ref_path.insert(0, upstream_prefix_);
                }
                if (ref_path.empty() || ref_path[0] != '/')
                {
                    ref_path.insert(0, 1, '/');
                }

                auto new_ref = std::format("{}://{}{}",
                                           upstream_scheme_,
                                           util::make_host_value(upstream_host_, port_, ssl_),
                                           ref_path);
                if (!r->encoded_query().empty())
                {
                    new_ref += std::format("?{}", std::string(r->encoded_query()));
                }
                if (!r->encoded_fragment().empty())
                {
                    new_ref += std::format("#{}", std::string(r->encoded_fragment()));
                }
                upstream_headers.set(http::field::referer, new_ref);
            }
        }

        upstream_headers_ = std::move(upstream_headers);
    }

    net::awaitable<bool>
    reverse_proxy_context::forward_request(request& req, response& resp)
    {
        if (interceptor_)
        {
            co_await interceptor_->on_upstream_request(req, upstream_headers_, upstream_url_);
        }

        client_ = co_await pool_->async_acquire(upstream_host_, port_, ssl_);
        if (!client_)
        {
            logger_->trace("[proxy] acquire client failed for {}:{}", upstream_host_, port_);
            resp.set_error_content(http::status::service_unavailable);
            co_return false;
        }

        writer_ = client_->create_lazy_request();

        if (auto rel_ec = co_await writer_->write_header(req.method(), upstream_target_, upstream_headers_); rel_ec)
        {
            logger_->trace("[proxy] write_header to {}:{} failed: {}", upstream_host_, port_, rel_ec.message());
            resp.set_error_content(upstream_error_to_status(rel_ec));
            co_return false;
        }

        while (!req.is_body_done())
        {
            std::size_t bytes;
            try
            {
                bytes = co_await req.read_some_raw(net::buffer(relay_buf_));
            }
            catch (boost::system::system_error const& e)
            {
                logger_->trace("[proxy] read request body failed: {}", e.what());
                resp.set_error_content(http::status::bad_request);
                co_return false;
            }
            auto more = !req.is_body_done();

            if (interceptor_)
            {
                co_await interceptor_->on_upstream_request_body(net::buffer(relay_buf_, bytes), more);
            }

            if (auto rel_ec = co_await writer_->write_body(net::buffer(relay_buf_, bytes), more); rel_ec)
            {
                logger_->trace("[proxy] write_body to {}:{} failed: {}", upstream_host_, port_, rel_ec.message());
                resp.set_error_content(upstream_error_to_status(rel_ec));
                co_return false;
            }
        }

        co_return true;
    }

    net::awaitable<bool>
    reverse_proxy_context::read_upstream_response(request& req, response& resp)
    {
        auto up_result = co_await writer_->read_response_lazy();
        if (up_result.has_error())
        {
            logger_->trace("[proxy] read_header from {}:{} failed: {}",
                           upstream_host_,
                           port_,
                           up_result.error().message());
            resp.set_error_content(upstream_error_to_status(up_result.error()));
            co_return false;
        }
        upstream_response_ = std::move(up_result.value());

        auto const& headers = upstream_response_.headers();
        auto const result = upstream_response_.result();
        logger_->debug("[proxy] {} {} <- {} {}",
                       req.method_string(),
                       req.target(),
                       static_cast<unsigned>(result),
                       upstream_host_);

        if (interceptor_)
        {
            co_await interceptor_->on_upstream_response(req, result, headers);
        }

        auto response_hdrs = http::fields(headers);
        // Strip hop-by-hop headers before relaying to client
        response_hdrs.erase(http::field::connection);
        response_hdrs.erase(http::field::keep_alive);
        response_hdrs.erase(http::field::te);
        response_hdrs.erase(http::field::trailer);
        response_hdrs.erase(http::field::upgrade);

        if (result >= http::status::moved_permanently && result <= http::status::permanent_redirect
            && result != http::status::not_modified)
        {
            auto upstream_base = util::make_url_value(upstream_host_, port_, ssl_);
            std::string location(response_hdrs[http::field::location]);
            if (location.starts_with(upstream_base))
            {
                response_hdrs.set(http::field::location, prefix_ + location.substr(upstream_base.size()));
            }
        }

        if (auto rel_ec = co_await resp.create_stream_writer()->write_header(result, response_hdrs); rel_ec)
        {
            logger_->trace("[proxy] write response header failed: {}", rel_ec.message());
            co_return false;
        }

        co_return true;
    }

    net::awaitable<void>
    reverse_proxy_context::relay_response(response& resp)
    {
        while (!upstream_response_.is_body_done())
        {
            auto bytes_result = co_await upstream_response_.read_some_raw(net::buffer(relay_buf_));
            if (bytes_result.has_error())
            {
                logger_->trace("[proxy] read response body failed: {}", bytes_result.error().message());
                co_return;
            }
            auto bytes = bytes_result.value();
            auto more = !upstream_response_.is_body_done();

            if (interceptor_)
            {
                co_await interceptor_->on_upstream_response_body(net::buffer(relay_buf_, bytes), more);
            }

            if (auto rel_ec = co_await resp.create_stream_writer()->write_body(net::buffer(relay_buf_, bytes), more);
                rel_ec)
            {
                logger_->trace("[proxy] write response body failed: {}", rel_ec.message());
                co_return;
            }
        }
        co_return;
    }
} // namespace httplib::server::detail
