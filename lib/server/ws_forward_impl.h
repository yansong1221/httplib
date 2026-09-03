#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server.hpp"
#include "httplib/server/websocket_conn.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/fields.hpp>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace httplib::client
{
    class ws_client;
}

namespace httplib::server::detail
{
    /**
     * \brief Per-connection WebSocket forward state.
     * \details Persisted on the handshake request for the whole forwarded session.
     * One object per connection; keeps the upstream client and interceptor alive and
     * lets the router-level message/close handlers and the connection abort path
     * reach them through a single request_data slot.
     */
    struct ws_forward_state
    {
        std::shared_ptr<client::ws_client> upstream;
        std::shared_ptr<ws_interceptor> interceptor;
    };

    using ws_forward_state_ptr = std::shared_ptr<ws_forward_state>;

    /**
     * \brief Per-connection WebSocket forward execution context.
     * \details Created once per forwarded connection inside the route's open
     * handler; never shared across connections. Mirrors reverse_proxy_context so
     * each stage stays small and failures funnel through one place.
     */
    class ws_forward_context
    {
      public:
        ws_forward_context(net::any_io_executor const& ex,
                           std::string prefix,
                           http_server::proxy_resolver resolver,
                           http_server::ws_interceptor_factory factory,
                           std::shared_ptr<spdlog::logger> logger);

        /// Entry point invoked from the route's open handler.
        net::awaitable<void> run(websocket_conn::weak_ptr wp);

        /// Downstream -> upstream relay (registered as the route message handler).
        static net::awaitable<void> send_to_upstream(websocket_conn::weak_ptr wp, std::string_view data, bool binary);

        /// Upstream teardown on client disconnect (registered as the route close handler).
        static net::awaitable<void> close_upstream(websocket_conn::weak_ptr wp);

      private:
        // ---- stages (each maps 1:1 to a block of the original open handler) ----

        /// Create the interceptor, resolve + parse the upstream URL, build the
        /// rewritten path and upstream headers. Closes the connection on failure.
        net::awaitable<bool> prepare_upstream(websocket_conn& conn);

      private:
        net::any_io_executor ex_;
        std::string prefix_;
        http_server::proxy_resolver resolver_;
        http_server::ws_interceptor_factory factory_;
        std::shared_ptr<spdlog::logger> logger_;

        // ---- per-connection state (set by prepare_upstream) ----
        std::shared_ptr<http_server::proxy_target> target_;
        std::shared_ptr<ws_interceptor> interceptor_;

        std::string upstream_host_;
        uint16_t upstream_port_ = 80;
        bool upstream_ssl_ = false;
        std::string upstream_scheme_;
        std::string upstream_target_;
        std::string upstream_url_;
        http::fields upstream_headers_ {};
    };
} // namespace httplib::server::detail
