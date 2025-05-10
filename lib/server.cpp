
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
using namespace std::chrono_literals;

class server::impl
{
public:
    impl(uint32_t num_threads)
        : pool(num_threads)
        , acceptor(pool)
        , router(this->option)
    {
    }

public:
    server::setting option;
    httplib::router router;
    net::thread_pool pool;
    tcp::acceptor acceptor;

    std::mutex session_mtx;
    std::unordered_set<std::shared_ptr<session>> session_map;
};

server::server(uint32_t num_threads)
    : impl_(new impl(num_threads))
{
}

server::~server()
{
    delete impl_;
}

net::any_io_executor server::get_executor() noexcept
{
    return impl_->pool.get_executor();
}


server::setting& server::option()
{
    return impl_->option;
}

server& server::listen(std::string_view host,
                       uint16_t port,
                       int backlog /*= net::socket_base::max_listen_connections*/)
{
    tcp::resolver resolver(impl_->pool);
    auto results = resolver.resolve(host, std::to_string(port));

    tcp::endpoint endp(*results.begin());
    impl_->acceptor.open(endp.protocol());
    impl_->acceptor.bind(endp);
    impl_->acceptor.listen(backlog);
    impl_->option.get_logger()->info(
        "Server Listen on: [{}:{}]", endp.address().to_string(), endp.port());
    return *this;
}

server& server::listen(uint16_t port, int backlog /*= net::socket_base::max_listen_connections*/)
{
    return listen("0.0.0.0", port, backlog);
}

void server::run()
{
    async_run();
    wait();
}

void server::async_run()
{
    net::co_spawn(
        impl_->pool,
        [this]() -> net::awaitable<void> {
            boost::system::error_code ec;
            for (;;) {
                tcp::socket sock(impl_->pool);
                co_await impl_->acceptor.async_accept(sock, net_awaitable[ec]);
                if (ec) {
                    impl_->option.get_logger()->trace("async_accept: {}", ec.message());
                    co_return;
                }
                auto session = std::make_shared<httplib::session>(
                    std::move(sock), impl_->option, impl_->router);
                net::co_spawn(
                    impl_->pool,
                    [this, session]() mutable -> net::awaitable<void> {
                        {
                            std::unique_lock<std::mutex> lck(impl_->session_mtx);
                            impl_->session_map.insert(session);
                        }
                        try {
                            co_await session->run();
                        }
                        catch (const std::exception& e) {
                            impl_->option.get_logger()->error("session::run() exception: {}",
                                                              e.what());
                        }
                        catch (...) {
                            impl_->option.get_logger()->error("session::run() unknown exception");
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

void server::wait()
{
    impl_->pool.wait();
    impl_->session_map.clear();
}

void server::stop()
{
    boost::system::error_code ec;
    impl_->acceptor.close(ec);
    {
        std::unique_lock<std::mutex> lck(impl_->session_mtx);
        for (const auto& v : impl_->session_map)
            v->abort();
    }
    impl_->pool.stop();
}
httplib::router& server::router()
{
    return impl_->router;
}

} // namespace httplib