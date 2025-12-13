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


#include <boost/cobalt/detached.hpp>
#include <boost/cobalt/generator.hpp>
#include <boost/cobalt/promise.hpp>
#include <boost/cobalt/spawn.hpp>
#include <boost/cobalt/task.hpp>
#include <boost/cobalt/wait_group.hpp>

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
    cobalt::task<boost::system::error_code> co_run();

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

private:
    cobalt::detached handle_accept(tcp::socket sock,
                                        boost::asio::executor_arg_t = {},
                                        cobalt::executor = cobalt::this_thread::get_executor());
    cobalt::promise<void> accept_socket(boost::asio::executor_arg_t = {},
                                        cobalt::executor = cobalt::this_thread::get_executor());

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

    struct SSLConfig
    {
        std::vector<uint8_t> cert_file;
        std::vector<uint8_t> key_file;
        std::string passwd;
    };

    std::optional<SSLConfig> ssl_conf_;

    friend class websocket_conn_impl;
    friend class session;
};

} // namespace httplib::server