#pragma once
#include "httplib/config.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "stream/http_stream.hpp"
#include "stream/websocket_stream.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <memory>
#include <boost/cobalt/task.hpp>

namespace httplib::server {
class http_server_impl;
class websocket_conn_impl;

class session : public std::enable_shared_from_this<session>
{
public:
    class task
    {
    public:
        virtual ~task()                                      = default;
        virtual cobalt::task<std::unique_ptr<task>> then() = 0;
        virtual void abort()                                 = 0;
    };
    class detect_ssl_task;
    class ssl_handshake_task;
    class http_task;
    class http_proxy_task;
    class websocket_task;

    explicit session(tcp::socket&& stream, http_server_impl& serv);
    ~session();

public:
    void abort();
    cobalt::task<void> run();

private:
    std::unique_ptr<task> task_;

    std::atomic_bool abort_ = false;
    std::mutex task_mtx_;
};

class session::detect_ssl_task : public session::task
{
public:
    explicit detect_ssl_task(tcp::socket&& stream, http_server_impl& sevr);
    ~detect_ssl_task();

public:
    void abort() override;
    cobalt::task<std::unique_ptr<task>> then() override;

private:
    http_server_impl& sevr_;
    http_stream::plain_stream stream_;
};


class session::http_task : public session::task
{
public:
    explicit http_task(http_stream&& stream,
                       beast::flat_buffer&& buffer,
                       http_server_impl& serv);

    cobalt::task<std::unique_ptr<task>> then() override;
    void abort() override;

private:
    cobalt::task<bool> async_write(request& req, response& resp);

private:
    http_server_impl& serv_;

    http_stream stream_;
    beast::flat_buffer buffer_;

    tcp::endpoint local_endpoint_;
    tcp::endpoint remote_endpoint_;
};

class session::websocket_task : public session::task
{
public:
    explicit websocket_task(std::unique_ptr<websocket_stream>&& stream,
                            request&& req,
                            http_server_impl& serv);

public:
    cobalt::task<std::unique_ptr<task>> then() override;
    void abort() override;

private:
    std::shared_ptr<websocket_conn_impl> conn_;
};

class session::http_proxy_task : public session::task
{
public:
    explicit http_proxy_task(http_stream&& stream,
                             request&& req,
                             http_server_impl& serv);

public:
    cobalt::task<std::unique_ptr<task>> then() override;
    void abort() override;

private:
    http_stream stream_;
    tcp::resolver resolver_;
    tcp::socket proxy_socket_;

    request req_;
    http_server_impl& serv_;
};

} // namespace httplib::server