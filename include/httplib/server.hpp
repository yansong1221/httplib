#pragma once
#include "config.hpp"
#include "websocket_conn.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/socket_base.hpp>
#include <filesystem>

namespace httplib {
class Router;
class Session;
} // namespace httplib

namespace httplib {
class Server {
public:
    struct SSLConfig {
        std::filesystem::path cert_file;
        std::filesystem::path key_file;
        std::string passwd;
    };
    struct Option {
        std::optional<SSLConfig> ssl_conf;
        std::shared_ptr<spdlog::logger> logger;
        std::chrono::steady_clock::duration read_timeout = std::chrono::seconds(30);
        std::chrono::steady_clock::duration write_timeout = std::chrono::seconds(30);

        websocket_conn::message_handler_type websocket_message_handler;
        websocket_conn::open_handler_type websocket_open_handler;
        websocket_conn::close_handler_type websocket_close_handler;
    };

public:
    explicit Server(uint32_t num_threads = std::thread::hardware_concurrency());
    ~Server();

    net::any_io_executor get_executor() noexcept;

    Option& option();

    Server& listen(std::string_view host,
                   uint16_t port,
                   int backlog = net::socket_base::max_listen_connections);
    void run();
    void async_run();
    void wait();
    void stop();

    Router& router();

private:
    class Impl;
    Impl* impl_;

    friend class Session;
    friend class Router;
};
} // namespace httplib