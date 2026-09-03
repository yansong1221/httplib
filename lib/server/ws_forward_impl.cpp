#include "ws_forward_impl.h"
#include "httplib/client/ws_client.hpp"
#include "httplib/util/misc.hpp"
#include "proxy_util.hpp"
#include "request_impl.hpp"
#include <boost/url.hpp>

namespace httplib::server::detail
{

    ws_forward_context::ws_forward_context(net::any_io_executor const& ex,
                                           std::string prefix,
                                           http_server::proxy_resolver resolver,
                                           http_server::ws_interceptor_factory factory,
                                           std::shared_ptr<spdlog::logger> logger)
        : ex_(ex)
        , prefix_(std::move(prefix))
        , resolver_(std::move(resolver))
        , factory_(std::move(factory))
        , logger_(std::move(logger))
    {
    }

    net::awaitable<void>
    ws_forward_context::run(websocket_conn::weak_ptr wp)
    {
        auto conn = wp.lock();
        if (!conn)
        {
            co_return;
        }

        if (!co_await prepare_upstream(*conn))
        {
            co_return;
        }

        request& req = conn->http_request();
        if (interceptor_)
        {
            co_await interceptor_->on_upstream_request(req, upstream_headers_, upstream_url_);
        }

        auto upstream = std::make_shared<client::ws_client>(ex_, upstream_host_, upstream_port_, upstream_ssl_);
        upstream->set_logger(logger_);

        auto ec = co_await upstream->async_run(
            upstream_target_,
            [conn = websocket_conn::weak_ptr(conn), interceptor = interceptor_](std::string_view data,
                                                                                bool binary) -> net::awaitable<void>
            {
                if (interceptor)
                {
                    co_await interceptor->on_upstream_recv(data, binary);
                }
                if (auto c = conn.lock())
                {
                    c->send(data, binary);
                }
                co_return;
            },
            [conn = websocket_conn::weak_ptr(conn)]() -> net::awaitable<void>
            {
                if (auto c = conn.lock())
                {
                    c->close();
                }
                co_return;
            },
            upstream_headers_);

        if (ec)
        {
            logger_->trace("[ws-forward] upstream connect failed: {}", ec.message());
            conn->close(ec.message());
            co_return;
        }

        auto state = std::make_shared<ws_forward_state>();
        state->upstream = std::move(upstream);
        state->interceptor = std::move(interceptor_);
        req.data().store<ws_forward_state_ptr>(std::move(state));
    }

    net::awaitable<bool>
    ws_forward_context::prepare_upstream(websocket_conn& conn)
    {
        request& req = conn.http_request();

        interceptor_ = factory_ ? factory_(req) : nullptr;

        target_ = co_await resolver_(req);
        if (!target_)
        {
            logger_->warn("[ws-forward] resolver returned null target");
            conn.close("resolver failed");
            co_return false;
        }
        auto const& url = target_->url();

        auto r = boost::urls::parse_uri(url);
        if (!r)
        {
            logger_->warn("[ws-forward] invalid upstream url: {}", url);
            conn.close("bad upstream");
            co_return false;
        }

        auto const& u = *r;
        upstream_host_ = std::string(u.host());
        upstream_scheme_ = std::string(u.scheme());
        upstream_ssl_ = (upstream_scheme_ == "wss" || upstream_scheme_ == "https");
        upstream_port_ = u.port_number() ? u.port_number() : (upstream_ssl_ ? 443 : 80);
        upstream_target_ = make_upstream_path(req.target(), prefix_, u.encoded_path());
        upstream_url_
            = util::make_url_value(upstream_host_, upstream_port_, upstream_ssl_, upstream_target_, upstream_scheme_);

        logger_->debug("[ws-forward] {} -> {}", req.target(), upstream_url_);

        upstream_headers_ = http::fields(req.base());
        upstream_headers_.erase(http::field::host);
        upstream_headers_.erase(http::field::sec_websocket_key);
        upstream_headers_.erase(http::field::sec_websocket_accept);
        upstream_headers_.erase(http::field::sec_websocket_version);
        upstream_headers_.erase(http::field::upgrade);
        upstream_headers_.erase(http::field::connection);
        upstream_headers_.set(http::field::host, util::make_host_value(upstream_host_, upstream_port_, upstream_ssl_));

        co_return true;
    }

    net::awaitable<void>
    ws_forward_context::send_to_upstream(websocket_conn::weak_ptr wp, std::string_view data, bool binary)
    {
        auto conn = wp.lock();
        if (!conn)
        {
            co_return;
        }

        request& req = conn->http_request();
        if (!req.data().has<ws_forward_state_ptr>())
        {
            conn->close();
            co_return;
        }

        auto state = req.data().fetch<ws_forward_state_ptr>();
        if (!state || !state->upstream)
        {
            conn->close();
            co_return;
        }

        if (state->interceptor)
        {
            co_await state->interceptor->on_upstream_send(data, binary);
        }

        auto ec = co_await state->upstream->async_send(std::string(data), binary);
        if (ec)
        {
            conn->close();
        }
    }

    net::awaitable<void>
    ws_forward_context::close_upstream(websocket_conn::weak_ptr wp)
    {
        auto conn = wp.lock();
        if (!conn)
        {
            co_return;
        }

        request& req = conn->http_request();
        if (!req.data().has<ws_forward_state_ptr>())
        {
            co_return;
        }

        auto state = req.data().fetch<ws_forward_state_ptr>();
        if (state && state->upstream)
        {
            co_await state->upstream->async_close();
        }
    }

} // namespace httplib::server::detail
