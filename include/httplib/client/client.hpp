#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/streaming/chunk_reader.hpp"
#include "httplib/streaming/chunk_writer.hpp"
#include "httplib/config.hpp"
#include "httplib/streaming/ndjson_reader.hpp"
#include "httplib/streaming/sse_reader.hpp"
#include <boost/asio/awaitable.hpp>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>

namespace httplib::client {

class HTTPLIB_API http_client
{
public:
    enum class timeout_policy
    {
        overall,
        step,
        never
    };


    using response        = http::response<body::any_body>;
    using request         = http::request<body::any_body>;
    using response_result = boost::system::result<response>;

    using chunked_write_handler_type = std::function<net::awaitable<void>(chunk_writer&)>;
    using chunked_read_handler_type =
        std::function<net::awaitable<void>(chunk_reader& reader, response& resp)>;

    using sse_read_handler_type = std::function<net::awaitable<void>(httplib::sse_reader& reader)>;
    using ndjson_read_handler_type =
        std::function<net::awaitable<void>(httplib::ndjson_reader& reader)>;

public:
    explicit http_client(net::io_context& ex,
                         std::string_view host,
                         uint16_t port,
                         bool ssl = false);
    explicit http_client(const net::any_io_executor& ex,
                         std::string_view host,
                         uint16_t port,
                         bool ssl = false);
    ~http_client();

    void set_timeout_policy(const timeout_policy& policy);

    void set_timeout(const std::chrono::steady_clock::duration& duration);

    std::string_view host() const;
    uint16_t port() const;
    bool is_use_ssl() const;

    std::shared_ptr<spdlog::logger> logger() const;
    void set_logger(std::shared_ptr<spdlog::logger> logger);

    void set_chunked_read_handler(chunked_read_handler_type&& handler);

    void set_sse_read_handler(sse_read_handler_type&& handler);

    void set_ndjson_read_handler(ndjson_read_handler_type&& handler);

    void set_max_redirects(int n);

public:
    // ---- HTTP method shorthands (no body) ----

    net::awaitable<response_result> async_get(std::string_view path,
                                              const html::query_params& params = {},
                                              const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_head(std::string_view path,
                                               const html::query_params& params = {},
                                               const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_post(std::string_view path,
                                               const html::query_params& params = {},
                                               const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_put(std::string_view path,
                                              const html::query_params& params = {},
                                              const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_patch(std::string_view path,
                                                const html::query_params& params = {},
                                                const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_del(std::string_view path,
                                              const html::query_params& params = {},
                                              const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_options(std::string_view path,
                                                  const html::query_params& params = {},
                                                  const http::fields& headers = http::fields());
    net::awaitable<response_result> async_connect(std::string_view path,
                                                  const html::query_params& params = {},
                                                  const http::fields& headers = http::fields());
    net::awaitable<response_result> async_trace(std::string_view path,
                                                const html::query_params& params = {},
                                                const http::fields& headers      = http::fields());

    // ---- HTTP method shorthands (with body) ----

    net::awaitable<response_result> async_post(std::string_view path,
                                               std::string_view body,
                                               const html::query_params& params = {},
                                               const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_post(std::string_view path,
                                               boost::json::value&& body,
                                               const html::query_params& params = {},
                                               const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_put(std::string_view path,
                                              std::string_view body,
                                              const html::query_params& params = {},
                                              const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_put(std::string_view path,
                                              boost::json::value&& body,
                                              const html::query_params& params = {},
                                              const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_patch(std::string_view path,
                                                std::string_view body,
                                                const html::query_params& params = {},
                                                const http::fields& headers      = http::fields());
    net::awaitable<response_result> async_patch(std::string_view path,
                                                boost::json::value&& body,
                                                const html::query_params& params = {},
                                                const http::fields& headers      = http::fields());

    response_result get(std::string_view path,
                        const html::query_params& params = {},
                        const http::fields& headers      = http::fields());
    response_result head(std::string_view path,
                         const html::query_params& params = {},
                         const http::fields& headers      = http::fields());
    response_result post(std::string_view path,
                         const html::query_params& params = {},
                         const http::fields& headers      = http::fields());
    response_result put(std::string_view path,
                        const html::query_params& params = {},
                        const http::fields& headers      = http::fields());
    response_result patch(std::string_view path,
                          const html::query_params& params = {},
                          const http::fields& headers      = http::fields());
    response_result del(std::string_view path,
                        const html::query_params& params = {},
                        const http::fields& headers      = http::fields());
    response_result options(std::string_view path,
                            const html::query_params& params = {},
                            const http::fields& headers      = http::fields());
    response_result connect(std::string_view path,
                            const html::query_params& params = {},
                            const http::fields& headers      = http::fields());
    response_result trace(std::string_view path,
                          const html::query_params& params = {},
                          const http::fields& headers      = http::fields());

    // ---- HTTP method shorthands (sync, with body) ----

    response_result post(std::string_view path,
                         std::string_view body,
                         const html::query_params& params = {},
                         const http::fields& headers      = http::fields());
    response_result post(std::string_view path,
                         boost::json::value&& body,
                         const html::query_params& params = {},
                         const http::fields& headers      = http::fields());
    response_result put(std::string_view path,
                        std::string_view body,
                        const html::query_params& params = {},
                        const http::fields& headers      = http::fields());
    response_result put(std::string_view path,
                        boost::json::value&& body,
                        const html::query_params& params = {},
                        const http::fields& headers      = http::fields());
    response_result patch(std::string_view path,
                          std::string_view body,
                          const html::query_params& params = {},
                          const http::fields& headers      = http::fields());
    response_result patch(std::string_view path,
                          boost::json::value&& body,
                          const html::query_params& params = {},
                          const http::fields& headers      = http::fields());

    // ---- async_send_request (core, body-type overloads) ----

    net::awaitable<response_result> async_send_request(
        http::verb method, std::string_view path, const http::fields& headers = http::fields());

    net::awaitable<response_result>
    async_send_request(http::verb method,
                       std::string_view path,
                       std::string_view body,
                       const http::fields& headers = http::fields());

    net::awaitable<response_result>
    async_send_request(http::verb method,
                       std::string_view path,
                       boost::json::value&& body,
                       const http::fields& headers = http::fields());

    net::awaitable<response_result>
    async_send_request(http::verb method,
                       std::string_view path,
                       html::form_data&& body,
                       const http::fields& headers = http::fields());

    net::awaitable<response_result>
    async_send_request(http::verb method,
                       std::string_view path,
                       html::query_params&& body,
                       const http::fields& headers = http::fields());

    net::awaitable<response_result>
    async_send_request(http::verb method,
                       std::string_view path,
                       const html::query_params& params,
                       const http::fields& headers = http::fields());

    net::awaitable<response_result>
    async_send_request(http::verb method,
                       std::string_view path,
                       const html::query_params& params,
                       std::string_view body,
                       const http::fields& headers = http::fields());

    net::awaitable<response_result>
    async_send_request(http::verb method,
                       std::string_view path,
                       const html::query_params& params,
                       boost::json::value&& body,
                       const http::fields& headers = http::fields());

    net::awaitable<response_result>
    async_send_request(http::verb method,
                       std::string_view path,
                       const html::query_params& params,
                       html::form_data&& body,
                       const http::fields& headers = http::fields());

    net::awaitable<response_result>
    async_send_request(http::verb method,
                       std::string_view path,
                       const html::query_params& params,
                       html::query_params&& body,
                       const http::fields& headers = http::fields());

    net::awaitable<response_result> async_send_file(http::verb method,
                                                    std::string_view path,
                                                    const fs::path& file_path,
                                                    const http::fields& headers = http::fields());

    net::awaitable<response_result> async_send_file(http::verb method,
                                                    std::string_view path,
                                                    const html::query_params& params,
                                                    const fs::path& file_path,
                                                    const http::fields& headers = http::fields());

    net::awaitable<response_result>
    async_send_chunked_request(http::verb method,
                               std::string_view path,
                               chunked_write_handler_type handler,
                               const html::query_params& params = {},
                               const http::fields& headers      = http::fields());

    response_result send_chunked_request(http::verb method,
                                         std::string_view path,
                                         chunked_write_handler_type handler,
                                         const html::query_params& params = {},
                                         const http::fields& headers      = http::fields());

    // ---- send_request (sync, body-type overloads) ----

    response_result send_request(http::verb method,
                                 std::string_view path,
                                 const http::fields& headers = http::fields());

    response_result send_request(http::verb method,
                                 std::string_view path,
                                 std::string_view body,
                                 const http::fields& headers = http::fields());

    response_result send_request(http::verb method,
                                 std::string_view path,
                                 boost::json::value&& body,
                                 const http::fields& headers = http::fields());

    response_result send_request(http::verb method,
                                 std::string_view path,
                                 html::form_data&& body,
                                 const http::fields& headers = http::fields());

    response_result send_request(http::verb method,
                                 std::string_view path,
                                 html::query_params&& body,
                                 const http::fields& headers = http::fields());

    response_result send_request(http::verb method,
                                 std::string_view path,
                                 const html::query_params& params,
                                 const http::fields& headers = http::fields());

    response_result send_request(http::verb method,
                                 std::string_view path,
                                 const html::query_params& params,
                                 std::string_view body,
                                 const http::fields& headers = http::fields());

    response_result send_request(http::verb method,
                                 std::string_view path,
                                 const html::query_params& params,
                                 boost::json::value&& body,
                                 const http::fields& headers = http::fields());

    response_result send_request(http::verb method,
                                 std::string_view path,
                                 const html::query_params& params,
                                 html::form_data&& body,
                                 const http::fields& headers = http::fields());

    response_result send_request(http::verb method,
                                 std::string_view path,
                                 const html::query_params& params,
                                 html::query_params&& body,
                                 const http::fields& headers = http::fields());

    response_result send_file(http::verb method,
                              std::string_view path,
                              const fs::path& file_path,
                              const http::fields& headers = http::fields());

    response_result send_file(http::verb method,
                              std::string_view path,
                              const html::query_params& params,
                              const fs::path& file_path,
                              const http::fields& headers = http::fields());

    // ---- Download ----

    net::awaitable<response_result> async_download(http::verb method,
                                                   std::string_view path,
                                                   const fs::path& save_path,
                                                   const http::fields& headers = http::fields());

    response_result download(http::verb method,
                             std::string_view path,
                             const fs::path& save_path,
                             const http::fields& headers = http::fields());

    void close();
    bool is_open() const;

private:
    class impl;
    std::unique_ptr<impl> impl_;
};
} // namespace httplib::client
