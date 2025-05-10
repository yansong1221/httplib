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
class server
{
public:
    struct setting;

public:
    explicit server(uint32_t num_threads = std::thread::hardware_concurrency());
    ~server();

    net::any_io_executor get_executor() noexcept;

    setting& option();

    server& listen(std::string_view host,
                   uint16_t port,
                   int backlog = net::socket_base::max_listen_connections);
    server& listen(uint16_t port, int backlog = net::socket_base::max_listen_connections);
    void run();
    void async_run();
    void wait();
    void stop();

    httplib::router& router();

private:
    class impl;
    impl* impl_;

    friend class session;
    friend class router;
};
} // namespace httplib