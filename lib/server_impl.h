#pragma once
#include "httplib/server.hpp"

#include "httplib/router.hpp"
#include "session.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/thread_pool.hpp>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <spdlog/spdlog.h>
#include <unordered_set>

namespace httplib {
class server_impl
{
public:
    explicit server_impl(const net::any_io_executor& ex);
    ~server_impl() = default;

public:
    net::any_io_executor get_executor() noexcept;

    void listen(std::string_view host,
                uint16_t port,
                int backlog = net::socket_base::max_listen_connections);

    void async_run();
    net::awaitable<boost::system::error_code> co_run();

    void stop();
    httplib::router& router();

    void set_read_timeout(const std::chrono::steady_clock::duration& dur);
    void set_write_timeout(const std::chrono::steady_clock::duration& dur);

    const std::chrono::steady_clock::duration& read_timeout() const;
    const std::chrono::steady_clock::duration& write_timeout() const;

    std::shared_ptr<spdlog::logger> get_logger() const;
    void set_logger(std::shared_ptr<spdlog::logger> logger);

    void use_ssl(const fs::path& cert_file, const fs::path& key_file, std::string passwd = {});

    void set_websocket_open_handler(websocket_conn::open_handler_type&& handle);
    void set_websocket_close_handler(websocket_conn::close_handler_type&& handle);
    void set_websocket_message_handler(websocket_conn::message_handler_type&& handle);

private:
    net::awaitable<void> handle_accept(tcp::socket&& sock);

private:
    net::any_io_executor ex_;

    httplib::router router_;
    tcp::acceptor acceptor_;

    std::mutex session_mtx_;
    std::unordered_set<std::shared_ptr<session>> session_map_;

    std::chrono::steady_clock::duration read_timeout_  = std::chrono::seconds(30);
    std::chrono::steady_clock::duration write_timeout_ = std::chrono::seconds(30);

    std::shared_ptr<spdlog::logger> default_logger_;
    std::shared_ptr<spdlog::logger> custom_logger_;

    websocket_conn::message_handler_type websocket_message_handler_;
    websocket_conn::open_handler_type websocket_open_handler_;
    websocket_conn::close_handler_type websocket_close_handler_;

    struct SSLConfig
    {
        std::filesystem::path cert_file;
        std::filesystem::path key_file;
        std::string passwd;
    };

    std::optional<SSLConfig> ssl_conf_;

    friend class websocket_conn_impl;
    friend class session;
};

} // namespace httplib