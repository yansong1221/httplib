#pragma once
#include "config.hpp"
#include "websocket_conn.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/socket_base.hpp>
#include <filesystem>

namespace httplib
{
class router;
class server
{
public:
    struct ssl_config
    {
        std::filesystem::path cert_file;
        std::filesystem::path key_file;
        std::string passwd;
    };

    explicit server(uint32_t num_threads = std::thread::hardware_concurrency());
    virtual ~server();

public:
    net::any_io_executor get_executor() noexcept;
    std::shared_ptr<spdlog::logger> get_logger() noexcept;
    void set_logger(std::shared_ptr<spdlog::logger> logger);

    server& listen(std::string_view host, uint16_t port, int backlog = net::socket_base::max_listen_connections);
    void run();
    void async_run();

public:
    void set_websocket_message_handler(websocket_conn::message_handler_type&& handler);
    void set_websocket_open_handler(websocket_conn::open_handler_type&& handler);
    void set_websocket_close_handler(websocket_conn::close_handler_type&& handler);
    router& get_router();

private:
    class impl;
    std::shared_ptr<impl> impl_;
};
} // namespace httplib