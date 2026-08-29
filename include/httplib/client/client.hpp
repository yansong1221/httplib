#pragma once
#include "httplib/client/client_fwd.hpp"
#include "httplib/client/request.hpp"
#include "httplib/client/response.hpp"
#include <boost/asio/awaitable.hpp>
#include <filesystem>
#include <memory>

namespace httplib::client
{

    class HTTPLIB_API http_client
    {
      public:
        enum class timeout_policy
        {
            overall,
            step,
            never
        };

        using response = client::response;
        using request = client::request;
        using response_result = boost::system::result<response>;

      public:
        explicit http_client(net::io_context& ex, std::string_view host, uint16_t port, bool ssl = false);
        explicit http_client(net::any_io_executor const& ex, std::string_view host, uint16_t port, bool ssl = false);
        explicit http_client(net::io_context& ex, std::string_view url);
        explicit http_client(net::any_io_executor const& ex, std::string_view url);
        ~http_client();

        void set_timeout_policy(timeout_policy const& policy);

        void set_timeout(std::chrono::steady_clock::duration const& duration);

        std::string_view host() const;
        uint16_t port() const;
        bool is_use_ssl() const;

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        void set_max_redirects(int n);
        void set_verify_ssl(bool verify);
        void set_ca_cert(std::string_view cert);

        net::any_io_executor get_executor() const;

      public:
        // ---- core send ----

        net::awaitable<response_result> async_send_request(request req);
        net::awaitable<response_result> async_send_request_lazy(request req);

        // ---- HTTP method shorthands (no body) ----

        net::awaitable<response_result> async_get(std::string_view path,
                                                  html::query_params const& params = {},
                                                  http::fields const& headers = http::fields());
        net::awaitable<response_result> async_head(std::string_view path,
                                                   html::query_params const& params = {},
                                                   http::fields const& headers = http::fields());
        net::awaitable<response_result> async_post(std::string_view path,
                                                   html::query_params const& params = {},
                                                   http::fields const& headers = http::fields());
        net::awaitable<response_result> async_put(std::string_view path,
                                                  html::query_params const& params = {},
                                                  http::fields const& headers = http::fields());
        net::awaitable<response_result> async_patch(std::string_view path,
                                                    html::query_params const& params = {},
                                                    http::fields const& headers = http::fields());
        net::awaitable<response_result> async_del(std::string_view path,
                                                  html::query_params const& params = {},
                                                  http::fields const& headers = http::fields());
        net::awaitable<response_result> async_options(std::string_view path,
                                                      html::query_params const& params = {},
                                                      http::fields const& headers = http::fields());

        // ---- HTTP method shorthands (with body) ----

        net::awaitable<response_result> async_post(std::string_view path,
                                                   std::string_view body,
                                                   html::query_params const& params = {},
                                                   http::fields const& headers = http::fields());
        net::awaitable<response_result> async_post(std::string_view path,
                                                   boost::json::value&& body,
                                                   html::query_params const& params = {},
                                                   http::fields const& headers = http::fields());
        net::awaitable<response_result> async_put(std::string_view path,
                                                  std::string_view body,
                                                  html::query_params const& params = {},
                                                  http::fields const& headers = http::fields());
        net::awaitable<response_result> async_put(std::string_view path,
                                                  boost::json::value&& body,
                                                  html::query_params const& params = {},
                                                  http::fields const& headers = http::fields());
        net::awaitable<response_result> async_patch(std::string_view path,
                                                    std::string_view body,
                                                    html::query_params const& params = {},
                                                    http::fields const& headers = http::fields());
        net::awaitable<response_result> async_patch(std::string_view path,
                                                    boost::json::value&& body,
                                                    html::query_params const& params = {},
                                                    http::fields const& headers = http::fields());

        // ---- download ----

        net::awaitable<response_result> async_download(http::verb method,
                                                       std::string_view path,
                                                       fs::path const& save_path,
                                                       http::fields const& headers = http::fields());

        std::shared_ptr<lazy_request> create_lazy_request();

        void close();
        bool is_open() const;
        bool has_active_session() const;
        bool is_alive() const;

      private:
        http_client(http_client const&) = delete;
        http_client& operator=(http_client const&) = delete;

        class impl;
        std::shared_ptr<impl> impl_;

        friend class ::httplib::client::response::impl;

        friend std::shared_ptr<impl>&
        get_impl(http_client& self)
        {
            return self.impl_;
        }
        friend std::shared_ptr<impl> const&
        get_impl(http_client const& self)
        {
            return self.impl_;
        }
    };
} // namespace httplib::client
