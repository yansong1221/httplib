#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/client/client_fwd.hpp"
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

        using response = http::response<body::any_body>;
        using request = http::request<body::any_body>;
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

      public:
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
        net::awaitable<response_result> async_connect(std::string_view path,
                                                      html::query_params const& params = {},
                                                      http::fields const& headers = http::fields());
        net::awaitable<response_result> async_trace(std::string_view path,
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

        response_result get(std::string_view path,
                            html::query_params const& params = {},
                            http::fields const& headers = http::fields());
        response_result head(std::string_view path,
                             html::query_params const& params = {},
                             http::fields const& headers = http::fields());
        response_result post(std::string_view path,
                             html::query_params const& params = {},
                             http::fields const& headers = http::fields());
        response_result put(std::string_view path,
                            html::query_params const& params = {},
                            http::fields const& headers = http::fields());
        response_result patch(std::string_view path,
                              html::query_params const& params = {},
                              http::fields const& headers = http::fields());
        response_result del(std::string_view path,
                            html::query_params const& params = {},
                            http::fields const& headers = http::fields());
        response_result options(std::string_view path,
                                html::query_params const& params = {},
                                http::fields const& headers = http::fields());
        response_result connect(std::string_view path,
                                html::query_params const& params = {},
                                http::fields const& headers = http::fields());
        response_result trace(std::string_view path,
                              html::query_params const& params = {},
                              http::fields const& headers = http::fields());

        // ---- HTTP method shorthands (sync, with body) ----

        response_result post(std::string_view path,
                             std::string_view body,
                             html::query_params const& params = {},
                             http::fields const& headers = http::fields());
        response_result post(std::string_view path,
                             boost::json::value&& body,
                             html::query_params const& params = {},
                             http::fields const& headers = http::fields());
        response_result put(std::string_view path,
                            std::string_view body,
                            html::query_params const& params = {},
                            http::fields const& headers = http::fields());
        response_result put(std::string_view path,
                            boost::json::value&& body,
                            html::query_params const& params = {},
                            http::fields const& headers = http::fields());
        response_result patch(std::string_view path,
                              std::string_view body,
                              html::query_params const& params = {},
                              http::fields const& headers = http::fields());
        response_result patch(std::string_view path,
                              boost::json::value&& body,
                              html::query_params const& params = {},
                              http::fields const& headers = http::fields());

        // ---- async_send_request (core, body-type overloads) ----

        net::awaitable<response_result> async_send_request(http::verb method,
                                                           std::string_view path,
                                                           http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_request(http::verb method,
                                                           std::string_view path,
                                                           std::string_view body,
                                                           http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_request(http::verb method,
                                                           std::string_view path,
                                                           boost::json::value&& body,
                                                           http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_request(http::verb method,
                                                           std::string_view path,
                                                           html::form_data&& body,
                                                           http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_request(http::verb method,
                                                           std::string_view path,
                                                           html::query_params&& body,
                                                           http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_request(http::verb method,
                                                           std::string_view path,
                                                           html::query_params const& params,
                                                           http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_request(http::verb method,
                                                           std::string_view path,
                                                           html::query_params const& params,
                                                           std::string_view body,
                                                           http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_request(http::verb method,
                                                           std::string_view path,
                                                           html::query_params const& params,
                                                           boost::json::value&& body,
                                                           http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_request(http::verb method,
                                                           std::string_view path,
                                                           html::query_params const& params,
                                                           html::form_data&& body,
                                                           http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_request(http::verb method,
                                                           std::string_view path,
                                                           html::query_params const& params,
                                                           html::query_params&& body,
                                                           http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_file(http::verb method,
                                                        std::string_view path,
                                                        fs::path const& file_path,
                                                        http::fields const& headers = http::fields());

        net::awaitable<response_result> async_send_file(http::verb method,
                                                        std::string_view path,
                                                        html::query_params const& params,
                                                        fs::path const& file_path,
                                                        http::fields const& headers = http::fields());

        std::shared_ptr<write_session> create_writer();
        std::shared_ptr<read_session> create_reader();

        std::unique_ptr<sse_reader> create_sse_reader();
        std::unique_ptr<ndjson_reader> create_ndjson_reader();

        // ---- send_request (sync, body-type overloads) ----

        response_result send_request(http::verb method,
                                     std::string_view path,
                                     http::fields const& headers = http::fields());

        response_result send_request(http::verb method,
                                     std::string_view path,
                                     std::string_view body,
                                     http::fields const& headers = http::fields());

        response_result send_request(http::verb method,
                                     std::string_view path,
                                     boost::json::value&& body,
                                     http::fields const& headers = http::fields());

        response_result send_request(http::verb method,
                                     std::string_view path,
                                     html::form_data&& body,
                                     http::fields const& headers = http::fields());

        response_result send_request(http::verb method,
                                     std::string_view path,
                                     html::query_params&& body,
                                     http::fields const& headers = http::fields());

        response_result send_request(http::verb method,
                                     std::string_view path,
                                     html::query_params const& params,
                                     http::fields const& headers = http::fields());

        response_result send_request(http::verb method,
                                     std::string_view path,
                                     html::query_params const& params,
                                     std::string_view body,
                                     http::fields const& headers = http::fields());

        response_result send_request(http::verb method,
                                     std::string_view path,
                                     html::query_params const& params,
                                     boost::json::value&& body,
                                     http::fields const& headers = http::fields());

        response_result send_request(http::verb method,
                                     std::string_view path,
                                     html::query_params const& params,
                                     html::form_data&& body,
                                     http::fields const& headers = http::fields());

        response_result send_request(http::verb method,
                                     std::string_view path,
                                     html::query_params const& params,
                                     html::query_params&& body,
                                     http::fields const& headers = http::fields());

        response_result send_file(http::verb method,
                                  std::string_view path,
                                  fs::path const& file_path,
                                  http::fields const& headers = http::fields());

        response_result send_file(http::verb method,
                                  std::string_view path,
                                  html::query_params const& params,
                                  fs::path const& file_path,
                                  http::fields const& headers = http::fields());

        // ---- Download ----

        net::awaitable<response_result> async_download(http::verb method,
                                                       std::string_view path,
                                                       fs::path const& save_path,
                                                       http::fields const& headers = http::fields());

        response_result download(http::verb method,
                                 std::string_view path,
                                 fs::path const& save_path,
                                 http::fields const& headers = http::fields());

        void close();
        bool is_open() const;
        bool has_active_session() const;

      private:
        http_client(http_client const&) = delete;
        http_client& operator=(http_client const&) = delete;

        class impl;
        std::unique_ptr<impl> impl_;
    };
} // namespace httplib::client
