#pragma once
#include "httplib/html/form_data.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include "httplib/util/async_event.hpp"
#include "router_impl.h"
#include "session.hpp"
#include "util/logging.hpp"
#include <atomic>
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
#include <vector>

namespace httplib::server
{
    class http_server::impl
        : public httplib::detail::logger
        , public std::enable_shared_from_this<http_server::impl>
    {
      public:
        explicit impl(net::any_io_executor const& ex);
        ~impl();

      public:
        void listen(std::string_view host, uint16_t port);

        std::future<boost::system::error_code> run();
        net::awaitable<boost::system::error_code> async_run();

        std::future<void> stop();
        net::awaitable<void> async_stop();
        bool is_open() const;

        router_impl& router();

        void set_read_timeout(std::chrono::steady_clock::duration const& dur);
        void set_write_timeout(std::chrono::steady_clock::duration const& dur);

        std::chrono::steady_clock::duration read_timeout() const;
        std::chrono::steady_clock::duration write_timeout() const;

        tcp::endpoint const& local_endpoint() const;

        void set_compress_content_types(http_server::compress_predicate predicate);
        bool should_compress_content_type(std::string_view content_type) const;

        void
        set_form_data_params(html::form_data::param const& params)
        {
            form_data_params_.store(std::make_shared<html::form_data::param>(params));
        }
        html::form_data::param
        form_data_params() const
        {
            return *form_data_params_.load();
        }

        void
        set_header_limit(std::uint32_t limit)
        {
            header_limit_.store(limit);
        }
        std::uint32_t
        header_limit() const
        {
            return header_limit_.load();
        }

        void
        set_body_limit(std::uint64_t limit)
        {
            body_limit_.store(limit);
        }
        std::uint64_t
        body_limit() const
        {
            return body_limit_.load();
        }

        void set_reverse_proxy(std::string_view location,
                               std::string_view upstream_url,
                               http_server::proxy_interceptor_factory factory);
        void set_reverse_proxy(std::string_view location,
                               std::shared_ptr<upstream_provider> provider,
                               http_server::proxy_interceptor_factory factory);
        void set_reverse_proxy(std::string_view location,
                               std::vector<upstream_backend> backends,
                               upstream_locator locator,
                               http_server::proxy_interceptor_factory factory);

        void set_ws_forward(std::string_view location,
                            std::string_view upstream_url,
                            http_server::ws_interceptor_factory factory);
        void set_ws_forward(std::string_view location,
                            std::shared_ptr<upstream_provider> provider,
                            http_server::ws_interceptor_factory factory);
        void set_ws_forward(std::string_view location,
                            std::vector<upstream_backend> backends,
                            upstream_locator locator,
                            http_server::ws_interceptor_factory factory);

        void use_ssl(net::const_buffer const& cert_file, net::const_buffer const& key_file, std::string passwd = {});
#ifdef HTTPLIB_ENABLED_SSL
        std::shared_ptr<ssl::context>
        ssl_context() const
        {
            return ssl_context_.load();
        }
#endif
      private:
        net::awaitable<boost::system::error_code> co_accept();

      private:
        net::strand<net::any_io_executor> strand_;
        static constexpr auto acceptor_count_ = 32;

        router_impl router_;
        tcp::acceptor acceptor_;
        tcp::endpoint local_endpoint_;

        /// 仅在 `strand_` 上访问：accept 时在此注册、会话结束时注销。
        std::unordered_set<std::shared_ptr<session>> sessions_;

        /// Notified when `sessions_` transitions to empty; awaited by
        /// `async_run()` while draining in-flight sessions.
        util::async_event session_event_;

        std::atomic<std::chrono::steady_clock::duration> read_timeout_ { std::chrono::seconds(30) };
        std::atomic<std::chrono::steady_clock::duration> write_timeout_ { std::chrono::seconds(30) };

        std::atomic<std::shared_ptr<http_server::compress_predicate>> compress_predicate_;

        std::atomic<std::shared_ptr<html::form_data::param>> form_data_params_ {
            std::make_shared<html::form_data::param>(html::form_data::param { .max_file_size = 10 * 1024 * 1024 })
        };

        std::atomic<std::uint32_t> header_limit_ = 65536;
        std::atomic<std::uint64_t> body_limit_ = 1024ULL * 1024 * 1024;
        std::atomic<bool> running_ = false;

        /// Closed by `async_run()` when it exits; awaited by `async_stop()`.
        util::async_event stop_event_;

#ifdef HTTPLIB_ENABLED_SSL
        std::atomic<std::shared_ptr<ssl::context>> ssl_context_;
#endif

        friend class websocket_conn_impl;
        friend class session;
    };

} // namespace httplib::server
