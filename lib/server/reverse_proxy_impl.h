#pragma once
#include "httplib/client/client_pool.hpp"
#include "httplib/client/lazy_request.hpp"
#include "httplib/client/response.hpp"
#include "httplib/server/proxy_interceptor.hpp"
#include "httplib/server/server.hpp"
#include "request_impl.hpp"
#include "response_impl.hpp"
#include <array>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace httplib::server::detail
{
    /**
     * \brief Per-request reverse proxy execution context.
     * \details Carries everything a single proxied request needs so the
     * orchestration in run() stays linear and each stage method is small.
     * One instance is created per request; never shared across requests.
     */
    class reverse_proxy_context
    {
      public:
        reverse_proxy_context(std::shared_ptr<client::http_client_pool> pool,
                              std::string prefix,
                              http_server::proxy_resolver resolver,
                              http_server::proxy_interceptor_factory factory,
                              std::shared_ptr<spdlog::logger> logger);

        /// Entry point called from the route handler.
        net::awaitable<void> run(request& req, response& resp);

      private:
        // ---- stages (each maps 1:1 to a block of the original handler) ----

        /// Create the interceptor, resolve + parse the upstream URL, build the upstream target path.
        net::awaitable<bool> prepare_upstream(request& req, response& resp);

        /// Copy base headers, rewrite Cookie / X-Forwarded-* / Referer.
        void build_upstream_headers(request& req, response& resp);

        /// Notify interceptor, acquire an upstream client, forward request (header + streaming body).
        net::awaitable<bool> forward_request(request& req, response& resp);

        /// Read upstream response headers, strip hop-by-hop, rewrite Location, write response header.
        net::awaitable<bool> read_upstream_response(request& req, response& resp);

        /// Relay the upstream response body back to the client.
        net::awaitable<void> relay_response(response& resp);

      private:
        std::shared_ptr<client::http_client_pool> pool_;
        std::string prefix_;
        http_server::proxy_resolver resolver_;
        http_server::proxy_interceptor_factory factory_;
        std::shared_ptr<spdlog::logger> logger_;

        // ---- per-request state (set by stages) ----
        std::shared_ptr<proxy_interceptor> interceptor_;
        std::shared_ptr<http_server::proxy_target> target_;

        std::string upstream_scheme_;
        std::string upstream_host_;
        uint16_t port_ = 80;
        bool ssl_ = false;
        std::string upstream_prefix_;
        std::string upstream_target_;
        std::string upstream_url_;

        http::fields upstream_headers_ {};
        client::http_client_pool::client_handle client_;
        std::shared_ptr<client::lazy_request> writer_;
        client::response upstream_response_;
        std::array<char, 8192> relay_buf_ {};
    };
} // namespace httplib::server::detail
