#pragma once
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include "router_impl.h"
#include "session.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <spdlog/spdlog.h>
#include <string_view>
#include <unordered_set>

namespace httplib::client
{
    class http_client_pool;
}

namespace httplib::server
{

    class http_server::impl
    {
      public:
        explicit impl(net::any_io_executor const& ex, http_server& owner);
        ~impl();

      public:
        net::any_io_executor get_executor() noexcept;

        void listen(std::string_view host, uint16_t port, int backlog = net::socket_base::max_listen_connections);

        std::shared_future<boost::system::error_code> run();
        net::awaitable<boost::system::error_code> async_run();

        std::shared_future<void> stop();
        net::awaitable<void> async_stop();

        router_impl& router();

        void set_read_timeout(std::chrono::steady_clock::duration const& dur);
        void set_write_timeout(std::chrono::steady_clock::duration const& dur);

        std::chrono::steady_clock::duration read_timeout() const;
        std::chrono::steady_clock::duration write_timeout() const;

        int acceptor_count() const;
        void set_acceptor_count(int n);
        int proxy_buffer_size() const;
        void set_proxy_buffer_size(int sz);

        tcp::endpoint local_endpoint() const;

        std::shared_ptr<spdlog::logger> logger() const;
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        void set_compress_content_types(std::function<bool(std::string_view)> predicate);
        bool should_compress_content_type(std::string_view content_type) const;

        void
        set_upload_dir(fs::path const& dir)
        {
            upload_dir_ = dir;
        }
        void
        set_upload_file_limit(std::uint64_t max_bytes)
        {
            upload_file_limit_ = max_bytes;
        }
        fs::path const&
        upload_dir() const
        {
            return upload_dir_;
        }
        uint64_t
        upload_file_limit() const
        {
            return upload_file_limit_;
        }

        void set_reverse_proxy(std::string_view location,
                               http_server::proxy_resolver resolver,
                               http_server::proxy_header_callback on_headers);

        void set_proxy_pool_size(size_t max_size);

        void use_ssl(net::const_buffer const& cert_file, net::const_buffer const& key_file, std::string passwd = {});
#ifdef HTTPLIB_ENABLED_SSL
        const std::shared_ptr<ssl::context>&
        ssl_context() const
        {
            return ssl_context_;
        }
#endif

      private:
        net::awaitable<boost::system::error_code> co_accept();
        net::awaitable<void> handle_accept(tcp::socket sock);

      private:
        net::any_io_executor ex_;

        http_server* owner_ = nullptr;
        int acceptor_count_ = 32;
        int proxy_buffer_size_ = 512 * 1024;

        router_impl router_;
        tcp::acceptor acceptor_;

        std::mutex session_mutex_;
        std::unordered_set<std::shared_ptr<session>> sessions_;

        std::chrono::steady_clock::duration read_timeout_ = std::chrono::seconds(30);
        std::chrono::steady_clock::duration write_timeout_ = std::chrono::seconds(30);

        std::shared_ptr<spdlog::logger> default_logger_;
        std::shared_ptr<spdlog::logger> custom_logger_;

        std::function<bool(std::string_view)> compress_content_type_predicate_;

        fs::path upload_dir_;
        std::uint64_t upload_file_limit_ = 10 * 1024 * 1024;

        std::unique_ptr<client::http_client_pool> proxy_pool_;

#ifdef HTTPLIB_ENABLED_SSL
        std::shared_ptr<ssl::context> ssl_context_;
#endif

        friend class websocket_conn_impl;
        friend class session;
    };

} // namespace httplib::server