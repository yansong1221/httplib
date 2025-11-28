#pragma once
#include "httplib/config.hpp"
#include "httplib/server/websocket_conn.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/socket_base.hpp>
#include <filesystem>


namespace httplib::server {

class router;
class session;
class http_server_impl;


class http_server
{
public:
    struct setting;

public:
    explicit http_server(net::io_context& ioc);
    explicit http_server(const net::any_io_executor& ex);
    ~http_server();

    net::any_io_executor get_executor() noexcept;

    http_server& listen(std::string_view host,
                        uint16_t port,
                        int backlog = net::socket_base::max_listen_connections);
    http_server& listen(uint16_t port, int backlog = net::socket_base::max_listen_connections);
    net::awaitable<boost::system::error_code> co_run();
    void async_run();
    void stop();

    router& router();

    tcp::endpoint local_endpoint() const;

    void set_read_timeout(const std::chrono::steady_clock::duration& dur);
    void set_write_timeout(const std::chrono::steady_clock::duration& dur);

    const std::chrono::steady_clock::duration& read_timeout() const;
    const std::chrono::steady_clock::duration& write_timeout() const;

    std::shared_ptr<spdlog::logger> get_logger() const;
    void set_logger(std::shared_ptr<spdlog::logger> logger);

    void use_ssl(const std::span<const char>& cert_file,
                 const std::span<const char>& key_file,
                 std::string passwd = {});
    void use_ssl_file(const fs::path& cert_file, const fs::path& key_file, std::string passwd = {});

private:
    std::unique_ptr<http_server_impl> impl_;
};
} // namespace httplib::server