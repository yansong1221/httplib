#pragma once
#include "config.hpp"
#include "websocket_conn.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/socket_base.hpp>
#include <filesystem>

namespace httplib {
class router;
class session;
} // namespace httplib

namespace httplib {

class server_impl;
class server
{
public:
    struct setting;

public:
    explicit server(uint32_t num_threads = std::thread::hardware_concurrency());
    ~server();

    net::any_io_executor get_executor() noexcept;

    server& listen(std::string_view host,
                   uint16_t port,
                   int backlog = net::socket_base::max_listen_connections);
    server& listen(uint16_t port, int backlog = net::socket_base::max_listen_connections);
    void run();
    void async_run();
    void wait();
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
    server_impl* impl_;
};
} // namespace httplib