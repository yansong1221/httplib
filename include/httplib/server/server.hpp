#pragma once
#include "httplib/config.hpp"
#include "httplib/server/proxy_interceptor.hpp"
#include "httplib/server/proxy_strategy.hpp"
#include "httplib/server/server_fwd.hpp"
#include "httplib/server/ws_interceptor.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/beast/http/fields.hpp>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::server
{
    class HTTPLIB_API http_server
    {
      public:
        class impl;

      public:
        explicit http_server(net::io_context& ioc);
        explicit http_server(net::any_io_executor const& ex);
        ~http_server();

        net::any_io_executor get_executor() noexcept;

        http_server& listen(std::string_view host,
                            uint16_t port,
                            int backlog = net::socket_base::max_listen_connections);
        http_server& listen(uint16_t port, int backlog = net::socket_base::max_listen_connections);

        std::shared_future<boost::system::error_code> run();
        net::awaitable<boost::system::error_code> async_run();

        void stop();
        net::awaitable<void> async_stop();

        httplib::server::router& router();

        tcp::endpoint local_endpoint() const;

        void set_read_timeout(std::chrono::steady_clock::duration const& dur);
        void set_write_timeout(std::chrono::steady_clock::duration const& dur);

        std::chrono::steady_clock::duration read_timeout() const;
        std::chrono::steady_clock::duration write_timeout() const;

        void set_acceptor_count(int n);
        void set_proxy_buffer_size(int sz);

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        void set_compress_content_types(std::function<bool(std::string_view)> predicate);

        void set_upload_dir(fs::path const& dir);
        void set_upload_file_limit(std::uint64_t max_bytes);

        void set_header_limit(std::uint32_t limit);
        void set_body_limit(std::uint64_t limit);

        class proxy_target
        {
          public:
            virtual ~proxy_target() = default;
            virtual std::string const& url() const = 0;
        };

        using proxy_resolver = std::function<net::awaitable<std::shared_ptr<proxy_target>>(request& req)>;
        using proxy_interceptor_factory = std::function<std::shared_ptr<proxy_interceptor>(request& req)>;
        using ws_interceptor_factory = std::function<std::shared_ptr<ws_interceptor>(request& req)>;

        void set_reverse_proxy(std::string_view location,
                               std::string_view url,
                               proxy_interceptor_factory factory = nullptr);
        void set_reverse_proxy(std::string_view location,
                               proxy_resolver resolver,
                               proxy_interceptor_factory factory = nullptr);
        void set_reverse_proxy(std::string_view location,
                               std::vector<upstream_backend> backends,
                               upstream_locator locator = upstream_locator::round_robin,
                               proxy_interceptor_factory factory = nullptr);

        void set_ws_forward(std::string_view location, std::string_view url, ws_interceptor_factory factory = nullptr);
        void set_ws_forward(std::string_view location,
                            proxy_resolver resolver,
                            ws_interceptor_factory factory = nullptr);

        void set_ssl(std::span<char const> const& cert_file,
                     std::span<char const> const& key_file,
                     std::string passwd = {});
        void set_ssl_file(fs::path const& cert_file, fs::path const& key_file, std::string passwd = {});

      private:
        http_server(http_server const&) = delete;
        http_server& operator=(http_server const&) = delete;

        std::shared_ptr<impl> impl_;
    };
} // namespace httplib::server