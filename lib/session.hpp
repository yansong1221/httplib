#pragma once
#include "httplib/config.hpp"
#include "httplib/server.hpp"
#include "stream/http_stream.hpp"
#include <boost/asio/awaitable.hpp>
#include <memory>

namespace httplib {
class server;
class router;


class session : public std::enable_shared_from_this<session> {
public:
    class task {
    public:
        virtual ~task() = default;
        virtual net::awaitable<std::unique_ptr<task>> then() = 0;
        virtual void abort() = 0;
    };

    explicit session(tcp::socket&& stream,
                     const server::setting& option,
                     httplib::router& router);
    ~session();

public:
    void abort();
    net::awaitable<void> run();

private:
    net::ip::tcp::endpoint remote_endpoint_;
    net::ip::tcp::endpoint local_endpoint_;

    std::unique_ptr<task> task_;
    const server::setting& option_;
    std::atomic_bool abort_ = false;
    std::mutex task_mtx_;
};
} // namespace httplib