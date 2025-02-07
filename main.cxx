
#include "server.hpp"
#include <filesystem>
#include <format>

int main()
{  // HTTP

    server svr;
    svr.listen("127.0.0.1", 8808);
    svr.set_websocket_open_handler([](httplib::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> {
        co_return;
    });
    svr.set_websocket_close_handler([](httplib::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> {
        co_return;
    });
    svr.set_websocket_message_handler([](httplib::websocket_conn::weak_ptr hdl, httplib::websocket_conn::message msg) -> boost::asio::awaitable<void> {
        auto conn = hdl.lock();
        conn->send_message(msg);
        co_return;
    });
    svr.run();
}