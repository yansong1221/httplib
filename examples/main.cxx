
#include "httplib/server.hpp"
#include <filesystem>
#include <format>
#include <iostream>
//日志切面
struct log_t {
    bool before(httplib::request &req, httplib::response &res) {
        start_ = std::chrono::steady_clock::now();
        return true;
    }

    bool after(httplib::request &req, httplib::response &res) {
        auto span = std::chrono::steady_clock::now() - start_;
        std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(span) << std::endl; 
        return true;
    }

private:
    std::chrono::steady_clock::time_point start_;
};
int main() { // HTTP

    httplib::server svr;
    svr.listen("127.0.0.1", 8808);
    svr.set_websocket_open_handler(
        [](httplib::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> { co_return; });
    svr.set_websocket_close_handler(
        [](httplib::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> { co_return; });
    svr.set_websocket_message_handler(
        [](httplib::websocket_conn::weak_ptr hdl,
           httplib::websocket_conn::message msg) -> boost::asio::awaitable<void> {
            auto conn = hdl.lock();
            conn->send_message(msg);
            co_return;
        });
    svr.set_http_handler<httplib::http::verb::post>(
        "/",
        [](httplib::request &req, httplib::response &resp) -> boost::asio::awaitable<void> {
            req.is_body_type<httplib::form_data_body>();

            resp.base().result(httplib::http::status::ok);
            resp.set_body<httplib::form_data_body>(req.body<httplib::form_data_body>());
            co_return;
        },
        log_t{});
    svr.set_mount_point("/", R"(D:\code\cinatra\build)");
    svr.run();
}