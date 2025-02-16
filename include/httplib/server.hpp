#pragma once

#include "httplib/router.h"
#include "proxy_conn.hpp"
#include "request.hpp"
#include "stream/variant_stream.hpp"
#include "websocket_conn.hpp"
#include <boost/asio/thread_pool.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <span>

#include <spdlog/spdlog.h> 
namespace httplib {

class server {

public:
    struct ssl_config {
        std::filesystem::path cert_file;
        std::filesystem::path key_file;
        std::string passwd;
    };

    explicit server(uint32_t num_threads = std::thread::hardware_concurrency());

public:
    auto get_executor() noexcept {
        return pool_.get_executor();
    }

    server &listen(std::string_view host, uint16_t port,
                   int backlog = net::socket_base::max_listen_connections);

    void run() {
        async_run();
        pool_.wait();
    }

    void async_run();

public:
    net::awaitable<void> do_listen();

private:
    std::shared_ptr<ssl::context> create_ssl_context();

    template<class Body>
    net::awaitable<void>
    async_read_http_body(http::request_parser<http::empty_body> &&header_parser,
                         stream::http_stream_variant_type &stream, beast::flat_buffer &buffer,
                         httplib::request &req, boost::system::error_code &ec) {

        http::request_parser<Body> body_parser(std::move(header_parser));

        while (!body_parser.is_done()) {
            stream.expires_after(std::chrono::seconds(30));
            co_await http::async_read_some(stream, buffer, body_parser, net_awaitable[ec]);
            stream.expires_never();
            if (ec) {
                logger_->trace("read http body failed: {}", ec.message());
                co_return;
            }
        }
        req = std::move(body_parser.release());
        co_return;
    }

    net::awaitable<void> do_session(tcp::socket sock);
    net::awaitable<void> handle_connect(stream::http_stream_variant_type http_variant_stream,
                                        http::request<http::empty_body> req);
    net::awaitable<void> handle_websocket(stream::http_stream_variant_type http_variant_stream,
                                          http::request<http::empty_body> req);

public:
    void set_websocket_message_handler(httplib::websocket_conn::message_handler_type &&handler) {
        websocket_message_handler_ = handler;
    }
    void set_websocket_open_handler(httplib::websocket_conn::open_handler_type &&handler) {
        websocket_open_handler_ = handler;
    }
    void set_websocket_close_handler(httplib::websocket_conn::close_handler_type &&handler) {
        websocket_close_handler_ = handler;
    }

    template<http::verb... method, typename Func, typename... Aspects>
    void set_http_handler(std::string key, Func handler, Aspects &&...asps) {
        static_assert(sizeof...(method) >= 1, "must set http_method");
        if constexpr (sizeof...(method) == 1) {
            (router_.set_http_handler<method>(std::move(key), std::move(handler),
                                              std::forward<Aspects>(asps)...),
             ...);
        } else {
            (router_.set_http_handler<method>(key, handler, std::forward<Aspects>(asps)...), ...);
        }
    }

    template<http::verb... method, typename Func, typename... Aspects>
    void set_http_handler(std::string key, Func handler, util::class_type_t<Func> &owner,
                          Aspects &&...asps) {
        static_assert(std::is_member_function_pointer_v<Func>, "must be member function");
        using return_type = typename util::function_traits<Func>::return_type;
        if constexpr (is_awaitable_v<return_type>) {
            std::function<net::awaitable<void>(httplib::request & req, httplib::response & resp)>
                f = std::bind(handler, &owner, std::placeholders::_1, std::placeholders::_2);
            set_http_handler<method...>(std::move(key), std::move(f),
                                        std::forward<Aspects>(asps)...);
        } else {
            std::function<void(httplib::request & req, httplib::response & resp)> f =
                std::bind(handler, &owner, std::placeholders::_1, std::placeholders::_2);
            set_http_handler<method...>(std::move(key), std::move(f),
                                        std::forward<Aspects>(asps)...);
        }
    }

    bool set_mount_point(const std::string &mount_point, const std::filesystem::path &dir,
                         const http::fields &headers = {}) {

        return router_.set_mount_point(mount_point, dir, headers);
    }
    bool remove_mount_point(const std::string &mount_point) {
        return router_.remove_mount_point(mount_point);
    }

private:
    std::shared_ptr<spdlog::logger> logger_;
    httplib::router router_;
    net::thread_pool pool_;
    tcp::acceptor acceptor_;
    std::optional<ssl_config> ssl_config_;

    httplib::websocket_conn::message_handler_type websocket_message_handler_;
    httplib::websocket_conn::open_handler_type websocket_open_handler_;
    httplib::websocket_conn::close_handler_type websocket_close_handler_;
};
} // namespace httplib