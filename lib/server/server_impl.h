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
#include <unordered_set>

namespace httplib::server {
class http_server_impl
{
public:
    explicit http_server_impl(const net::any_io_executor& ex);
    ~http_server_impl() = default;

public:
    net::any_io_executor get_executor() noexcept;

    void listen(std::string_view host,
                uint16_t port,
                int backlog = net::socket_base::max_listen_connections);

    void async_run();
    net::awaitable<boost::system::error_code> co_run();

    void stop();
    router_impl& router();

    void set_read_timeout(const std::chrono::steady_clock::duration& dur);
    void set_write_timeout(const std::chrono::steady_clock::duration& dur);

    const std::chrono::steady_clock::duration& read_timeout() const;
    const std::chrono::steady_clock::duration& write_timeout() const;

    tcp::endpoint local_endpoint() const;

    std::shared_ptr<spdlog::logger> get_logger() const;
    void set_logger(std::shared_ptr<spdlog::logger> logger);

    void use_ssl(const net::const_buffer& cert_file,
                 const net::const_buffer& key_file,
                 std::string passwd = {});
#ifdef HTTPLIB_ENABLED_SSL
    const std::shared_ptr<ssl::context>& ssl_context() const { return ssl_context_; }
#endif

private:
    net::awaitable<boost::system::error_code> co_accept();
    net::awaitable<void> handle_accept(tcp::socket sock);

private:
    net::any_io_executor ex_;

    router_impl router_;
    tcp::acceptor acceptor_;

    std::mutex session_mutex_;
    std::unordered_set<std::shared_ptr<session>> session_map_;

    std::chrono::steady_clock::duration read_timeout_  = std::chrono::seconds(30);
    std::chrono::steady_clock::duration write_timeout_ = std::chrono::seconds(30);

    std::shared_ptr<spdlog::logger> default_logger_;
    std::shared_ptr<spdlog::logger> custom_logger_;

#ifdef HTTPLIB_ENABLED_SSL
    std::shared_ptr<ssl::context> ssl_context_;
#endif

    friend class websocket_conn_impl;
    friend class session;
};

} // namespace httplib::server