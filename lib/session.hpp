#pragma once
#include "httplib/config.hpp"
#include "httplib/server.hpp"
#include "stream/http_stream.hpp"
#include <boost/asio/awaitable.hpp>
#include <memory>

namespace httplib {
class Server;
class Router;


class Session : public std::enable_shared_from_this<Session> {
public:
    class Task {
    public:
        virtual ~Task() = default;
        virtual net::awaitable<std::unique_ptr<Task>> then() = 0;
        virtual void abort() = 0;
    };

    explicit Session(tcp::socket&& stream,
                     const Server::Option& option,
                     httplib::Router& router);
    ~Session();

public:
    void abort();
    net::awaitable<void> run();

private:
    net::ip::tcp::endpoint remote_endpoint_;
    net::ip::tcp::endpoint local_endpoint_;

    std::unique_ptr<Task> task_;
    const Server::Option& option_;
    std::atomic_bool abort_ = false;
    std::mutex task_mtx_;
};
} // namespace httplib