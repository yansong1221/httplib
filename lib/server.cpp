
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
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <unordered_set>

namespace httplib {
using namespace std::chrono_literals;

class Server::Impl {
public:
    Impl(uint32_t num_threads) : pool(num_threads), acceptor(pool) { }

public:
    Server::Option option;
    std::unique_ptr<Router> router;
    net::thread_pool pool;
    tcp::acceptor acceptor;

    websocket_conn::message_handler_type websocket_message_handler_;
    websocket_conn::open_handler_type websocket_open_handler_;
    websocket_conn::close_handler_type websocket_close_handler_;

    std::mutex session_mtx;
    std::unordered_set<std::shared_ptr<Session>> session_map;
};

Server::Server(uint32_t num_threads) : impl_(new Impl(num_threads))
{
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::sinks_init_list sink_list = {console_sink};
    impl_->option.logger = std::make_shared<spdlog::logger>("httplib.server", sink_list);
    impl_->option.logger->set_level(spdlog::level::info);

    impl_->router = std::make_unique<Router>(impl_->option);
}

Server::~Server() { delete impl_; }

net::any_io_executor Server::get_executor() noexcept
{
    return impl_->pool.get_executor();
}


Server::Option& Server::option() { return impl_->option; }

Server& Server::listen(std::string_view host,
                       uint16_t port,
                       int backlog /*= net::socket_base::max_listen_connections*/)
{
    tcp::resolver resolver(impl_->pool);
    auto results = resolver.resolve(host, std::to_string(port));

    tcp::endpoint endp(*results.begin());
    impl_->acceptor.open(endp.protocol());
    impl_->acceptor.bind(endp);
    impl_->acceptor.listen(backlog);
    impl_->option.logger->info(
        "Server Listen on: [{}:{}]", endp.address().to_string(), endp.port());
    return *this;
}

void Server::run()
{
    async_run();
    wait();
}

void Server::async_run()
{
    net::co_spawn(
        impl_->pool,
        [this]() -> net::awaitable<void> {
            boost::system::error_code ec;
            for (;;) {
                tcp::socket sock(impl_->pool);
                co_await impl_->acceptor.async_accept(sock, net_awaitable[ec]);
                if (ec) {
                    impl_->option.logger->trace("async_accept: {}", ec.message());
                    co_return;
                }
                auto session = std::make_shared<Session>(
                    std::move(sock), impl_->option, *impl_->router);
                net::co_spawn(
                    impl_->pool,
                    [this, session]() mutable -> net::awaitable<void> {
                        {
                            std::unique_lock<std::mutex> lck(impl_->session_mtx);
                            impl_->session_map.insert(session);
                        }
                        try {
                            co_await session->run();
                        } catch (const std::exception& e) {
                            impl_->option.logger->error("session::run() exception: {}",
                                                        e.what());
                        }
                        {
                            std::unique_lock<std::mutex> lck(impl_->session_mtx);
                            impl_->session_map.erase(session);
                        }
                    },
                    net::detached);
            }
        },
        net::detached);
}

void Server::wait() { impl_->pool.wait(); }

void Server::stop()
{
    boost::system::error_code ec;
    impl_->acceptor.close(ec);
    {
        std::unique_lock<std::mutex> lck(impl_->session_mtx);
        auto sessions = impl_->session_map;
        lck.unlock();
        for (const auto& v : sessions)
            v->abort();
    }
    impl_->pool.stop();
}

void Server::set_websocket_message_handler(
    httplib::websocket_conn::message_handler_type&& handler)
{
    impl_->websocket_message_handler_ = handler;
}
void Server::set_websocket_open_handler(
    httplib::websocket_conn::open_handler_type&& handler)
{
    impl_->websocket_open_handler_ = handler;
}
void Server::set_websocket_close_handler(
    httplib::websocket_conn::close_handler_type&& handler)
{
    impl_->websocket_close_handler_ = handler;
}
Router& Server::get_router() { return *impl_->router; }

} // namespace httplib