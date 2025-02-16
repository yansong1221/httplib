#pragma once
#include "stream/websocket_stream.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <queue>
#include <span>


namespace httplib {

class websocket_conn : public std::enable_shared_from_this<websocket_conn> {
public:
    class message {
    public:
        enum class data_type {
            text,
            binary
        };

    public:
        explicit message(std::string &&payload, data_type type = data_type::text)
            : payload_(std::move(payload)), type_(type){};
        explicit message(std::string_view payload, data_type type = data_type::text)
            : payload_(payload), type_(type){};
        message(const message &) = default;
        message(message &&) = default;
        message &operator=(message &&) = default;
        message &operator=(const message &) = default;

    public:
        const std::string &payload() const {
            return payload_;
        }
        data_type type() const {
            return type_;
        }

    private:
        std::string payload_;
        data_type type_;
    };

    using weak_ptr = std::weak_ptr<websocket_conn>;

    using open_handler_type = std::function<net::awaitable<void>(websocket_conn::weak_ptr)>;
    using close_handler_type = std::function<net::awaitable<void>(websocket_conn::weak_ptr)>;
    using message_handler_type =
        std::function<net::awaitable<void>(websocket_conn::weak_ptr, message)>;

public:
    websocket_conn(std::shared_ptr<spdlog::logger> logger,
                   stream::http_stream_variant_type &&stream);

    void set_message_handler(message_handler_type handler) {
        message_handler_ = handler;
    }
    void set_open_handler(open_handler_type handler) {
        open_handler_ = handler;
    }
    void set_close_handler(close_handler_type handler) {
        close_handler_ = handler;
    }
    void send_message(const message &msg) {
        send_message(message(msg));
    }
    void send_message(message &&msg);
    void close();

public:
    net::awaitable<void> process_write_data();
    net::awaitable<void> run(const http::request<http::empty_body> &req);

private:
    net::strand<net::any_io_executor> strand_;
    std::shared_ptr<spdlog::logger> logger_;
    std::unique_ptr<stream::ws_stream_variant_type> ws_;

    std::queue<message> send_que_;

    message_handler_type message_handler_;
    open_handler_type open_handler_;
    close_handler_type close_handler_;
};

} // namespace httplib