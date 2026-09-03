#include "ws_forward_impl.h"
#include "httplib/client/ws_client.hpp"
#include "httplib/util/misc.hpp"
#include "proxy_util.hpp"

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
            co_await interceptor_->on_upstream_request(req, upstream_headers_, upstream_.url);
        }

        auto upstream = std::make_shared<client::ws_client>(ex_, upstream_.host, upstream_.port, upstream_.ssl);
        upstream->set_logger(logger_);

        auto ec = co_await upstream->async_run(
            upstream_.target_path,
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

        auto result = co_await resolve_upstream(resolver_, req, prefix_, true);
        if (result.rc == upstream_resolve_rc::no_target)
        {
            logger_->warn("[ws-forward] resolver returned null target");
            conn.close("resolver failed");
            co_return false;
        }
        if (result.rc == upstream_resolve_rc::bad_url)
        {
            logger_->warn("[ws-forward] invalid upstream url: {}", result.value.raw_url);
            conn.close("bad upstream");
            co_return false;
        }

        upstream_ = std::move(result.value);

        logger_->debug("[ws-forward] {} -> {}", req.target(), upstream_.url);

        upstream_headers_ = http::fields(req.base());
        upstream_headers_.erase(http::field::host);
        upstream_headers_.erase(http::field::sec_websocket_key);
        upstream_headers_.erase(http::field::sec_websocket_accept);
        upstream_headers_.erase(http::field::sec_websocket_version);
        upstream_headers_.erase(http::field::upgrade);
        upstream_headers_.erase(http::field::connection);
        upstream_headers_.set(http::field::host, util::make_host_value(upstream_.host, upstream_.port, upstream_.ssl));

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
