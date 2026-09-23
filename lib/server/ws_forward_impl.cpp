#include "ws_forward_impl.h"
#include "httplib/client/ws_client.hpp"
#include "httplib/util/misc.hpp"
#include "proxy_util.hpp"
#include "server_impl.h"

namespace httplib::server::detail
{

    ws_forward_context::ws_forward_context(std::string prefix,
                                           std::shared_ptr<upstream_provider> provider,
                                           http_server::ws_interceptor_factory factory,
                                           std::shared_ptr<http_server::impl> ser)
        : prefix_(std::move(prefix))
        , provider_(std::move(provider))
        , factory_(std::move(factory))
        , ser_(std::move(ser))
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
            co_await interceptor_->on_upstream_request(req, upstream_headers_, upstream_.url);
        }

        auto upstream
            = std::make_shared<client::ws_client>(conn->get_executor(),
                                                  upstream_.host,
                                                  upstream_.port,
                                                  upstream_.ssl ? client::scheme::tls : client::scheme::plain);
        upstream->set_logger(get_logger());

        boost::system::error_code ec;
        co_await upstream->async_run(
            upstream_.target_path,
            upstream_headers_,
            [conn = websocket_conn::weak_ptr(conn),
             interceptor = interceptor_](websocket_message msg) -> net::awaitable<void>
            {
                if (interceptor)
                {
                    co_await interceptor->on_upstream_recv(msg.view(), msg.is_binary());
                }
                if (auto c = conn.lock())
                {
                    c->send(std::move(msg));
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
            ec);

        if (ec)
        {
            get_logger()->trace("[ws-forward] upstream connect failed: {}", ec.message());
            conn->close(ec.message());
            co_return;
        }

        auto state = std::make_shared<ws_forward_state>();
        state->upstream = std::move(upstream);
        state->interceptor = std::move(interceptor_);
        state->provider = provider_;
        state->url = upstream_.raw_url;
        if (provider_)
        {
            provider_->on_acquired(upstream_.raw_url);
        }
        req.data().store<ws_forward_state_ptr>(std::move(state));
    }

    net::awaitable<bool>
    ws_forward_context::prepare_upstream(websocket_conn& conn)
    {
        request& req = conn.http_request();

        interceptor_ = factory_ ? factory_(req) : nullptr;

        auto result = co_await resolve_upstream(provider_, req, prefix_, true);
        if (result.rc == upstream_resolve_rc::no_target)
        {
            get_logger()->warn("[ws-forward] provider is null");
            conn.close("resolver failed");
            co_return false;
        }
        if (result.rc == upstream_resolve_rc::bad_url)
        {
            get_logger()->warn("[ws-forward] invalid upstream url: {}", result.value.raw_url);
            conn.close("bad upstream");
            co_return false;
        }

        upstream_ = std::move(result.value);

        get_logger()->debug("[ws-forward] {} -> {}", req.target(), upstream_.url);

        upstream_headers_ = http::fields(req.base());
        upstream_headers_.erase(http::field::host);
        upstream_headers_.erase(http::field::sec_websocket_key);
        upstream_headers_.erase(http::field::sec_websocket_accept);
        upstream_headers_.erase(http::field::sec_websocket_version);
        upstream_headers_.erase(http::field::upgrade);
        upstream_headers_.erase(http::field::connection);
        upstream_headers_.set(http::field::host,
                              util::make_host_value(upstream_.host,
                                                    upstream_.port,
                                                    upstream_.ssl ? client::scheme::tls : client::scheme::plain));

        co_return true;
    }

    net::awaitable<void>
    ws_forward_context::send_to_upstream(websocket_conn::weak_ptr wp, const websocket_message& msg)
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
            co_await state->interceptor->on_upstream_send(msg.view(), msg.is_binary());
        }

        boost::system::error_code ec;
        co_await state->upstream->async_send(msg, ec);
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
            boost::system::error_code ec;
            co_await state->upstream->async_close(ec);
        }
        release_upstream_accounting(state);
    }

    std::shared_ptr<spdlog::logger>
    ws_forward_context::get_logger() const
    {
        return ser_->get_logger();
    }

} // namespace httplib::server::detail
