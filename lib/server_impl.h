#pragma once
#include "httplib/server.hpp"

#include "httplib/router.hpp"
#include "httplib/setting.hpp"
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
class server::impl
{
public:
    explicit impl(server& self, uint32_t num_threads);
    ~impl() = default;

public:
    net::any_io_executor get_executor() noexcept;
    setting& option();

    void listen(std::string_view host,
                uint16_t port,
                int backlog = net::socket_base::max_listen_connections);

    void async_run();
    void wait();

    void stop();
    httplib::router& router();

    void set_read_timeout(const std::chrono::steady_clock::duration& dur);
    void set_write_timeout(const std::chrono::steady_clock::duration& dur);

    const std::chrono::steady_clock::duration& read_timeout() const;
    const std::chrono::steady_clock::duration& write_timeout() const;

private:
    net::awaitable<void> accept_loop();
    net::awaitable<void> handle_accept(tcp::socket&& sock);

private:
    server& self_;

    net::thread_pool pool_;

    server::setting option_;
    httplib::router router_;
    tcp::acceptor acceptor_;

    std::mutex session_mtx_;
    std::unordered_set<std::shared_ptr<session>> session_map_;

    std::chrono::steady_clock::duration read_timeout_  = std::chrono::seconds(30);
    std::chrono::steady_clock::duration write_timeout_ = std::chrono::seconds(30);
};

} // namespace httplib