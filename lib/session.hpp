#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>

namespace httplib {
class server;

class session : public std::enable_shared_from_this<session>
{
public:
    class task
    {
    public:
        virtual ~task()                                      = default;
        virtual net::awaitable<std::unique_ptr<task>> then() = 0;
        virtual void abort()                                 = 0;
    };

    explicit session(tcp::socket&& stream, server& serv);
    ~session();

public:
    void abort();
    net::awaitable<void> run();

private:
    std::unique_ptr<task> task_;

    std::atomic_bool abort_ = false;
    std::mutex task_mtx_;
};
} // namespace httplib