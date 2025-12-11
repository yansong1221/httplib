
#include "httplib/client/client.hpp"
#include "httplib/client/multi_client_pool.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/thread_pool.hpp>
#include <boost/json.hpp>
#include <filesystem>
#include <format>
#include <iostream>
// 日志切面
struct log_t
{
    httplib::net::awaitable<bool> before(httplib::server::request& req,
                                         httplib::server::response& res)
    {
        // start_ = std::chrono::steady_clock::now();
        co_return true;
    }

    bool after(httplib::server::request& req, httplib::server::response& res) { return true; }

private:
    std::chrono::steady_clock::time_point start_;
};

struct test
{
    httplib::net::awaitable<void> get(httplib::server::request& req,
                                      httplib::server::response& resp)
    {
        const auto& req_json = req.body().as<httplib::body::json_body>();

        spdlog::info(boost::json::serialize(req_json));

        using namespace std::string_view_literals;
        resp.set_empty_content(httplib::http::status::ok);
        co_return;
    }
};

int main()
{ // HTTP
    using namespace std::string_view_literals;
    boost::asio::thread_pool pool(std::thread::hardware_concurrency());
    httplib::server::http_server svr(pool.get_executor());

    auto& router = svr.router();

    svr.get_logger()->set_level(spdlog::level::info);
    // svr.use_ssl(R"(D:\code\httplib\lib\server.crt)", R"(D:\code\httplib\lib\server.key)",
    // "test");

    svr.listen("0.0.0.0", 18808);

    auto client_pool =
        std::make_shared<httplib::client::multi_http_client_pool>(pool.get_executor());

    router.set_ws_handler(
        "/ws",
        [](httplib::server::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> {
            co_return;
        },
        [](httplib::server::websocket_conn::weak_ptr conn,
           std::string_view msg,
           bool binary) -> void { spdlog::info("msg: {}", msg); },
        [](httplib::server::websocket_conn::weak_ptr conn) {

        });
    router.set_ws_handler(
        "/ws/hello",
        [](httplib::server::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> {
            co_return;
        },
        [](httplib::server::websocket_conn::weak_ptr conn,
           std::string_view msg,
           bool binary) -> boost::asio::awaitable<void> {
            spdlog::info("msg hello: {}", msg);
            auto hdl = conn.lock();
            hdl->send_message(msg);
            // hdl->close();
            co_return;
        },
        [](httplib::server::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> {
            spdlog::info("111111111");
            co_return;
        });

    router.set_http_handler<httplib::http::verb::get>(
        "/hello",
        [&](httplib::server::request& req,
            httplib::server::response& resp) -> boost::asio::awaitable<void> {
            resp.set_stream_content(
                [](httplib::beast::flat_buffer& buffer,
                   boost::system::error_code& ec) -> boost::asio::awaitable<bool> {
                    static std::atomic_int32_t count = 0;
                    std::string data = std::format("hello stream content {}\n", count++);
                    buffer.commit(boost::asio::buffer_copy(buffer.prepare(data.size()),
                                                           boost::asio::buffer(data)));
                    if (count > 10)
                        co_return false;
                    co_return true;
                },
                "text/html");
            co_return;
        },
        log_t {});

    test tt;
    router.set_http_handler<httplib::http::verb::post>("/test", &test::get, tt, log_t {});
    // router.set_http_handler<httplib::http::verb::get>(
    //     "/close", [&](httplib::request& req, httplib::response& resp) { svr.stop(); });
    // router.set_http_handler<httplib::http::verb::post>(
    //     "/json",
    //     [](httplib::request& req, httplib::response& resp) -> httplib::net::awaitable<void> {
    //         auto& doc = req.body().as<httplib::body::json_body>();

    //        /*           const auto &obj = doc.get_object();
    //        for (const auto &item : obj.at("statuses").as_array()) {
    //            std::string_view created_at = item.at("created_at").as_string();
    //            resp.set_string_content(std::string(created_at), "text/html");
    //            co_return;
    //        }*/
    //        resp.set_json_content(doc);
    //        co_return;
    //    },
    //    log_t {});

    // router.set_http_handler<httplib::http::verb::post>(
    //     "/x-www-from-urlencoded",
    //     [](httplib::request& req, httplib::response& resp) -> httplib::net::awaitable<void> {
    //         auto& doc = req.body().as<httplib::body::query_params_body>();

    //        /*           const auto &obj = doc.get_object();
    //        for (const auto &item : obj.at("statuses").as_array()) {
    //            std::string_view created_at = item.at("created_at").as_string();
    //            resp.set_string_content(std::string(created_at), "text/html");
    //            co_return;
    //        }*/
    //        resp.set_error_content(httplib::http::status::internal_server_error);
    //        co_return;
    //    },
    //    log_t {});
    //// router.set_default_handler(
    ////     [](httplib::request& req, httplib::response& resp) ->
    ////     httplib::net::awaitable<void>
    ////     {
    ////         httplib::client cli(co_await httplib::net::this_coro::executor,
    ////         "127.0.0.1", 8888); cli.set_use_ssl(false);

    ////        boost::json::object doc;
    ////        doc["hello"] = "world";
    ////        auto result = co_await cli.async_post("/json", doc);
    ////        if (!result)
    ////        {
    ////            resp.set_error_content(httplib::http::status::bad_gateway);
    ////            co_return;
    ////        }
    ////        resp = std::move(*result);
    ////        // resp.set_string_content("hello"sv, "text/html");
    ////        co_return;
    ////    });
    //// svr.set_http_handler<httplib::http::verb::post, httplib::http::verb::get>(
    ////     "/hello/:w",
    ////     [](httplib::request &req, httplib::response &resp) {
    ////         req.is_body_type<httplib::body::form_data_body>();

    ////        resp.base().result(httplib::http::status::ok);
    ////        resp.set_string_content("1000", "text/html");
    ////        return;
    ////    },
    ////    log_t{});
    //// svr.set_http_handler<httplib::http::verb::post>(
    ////    "/",
    ////    [](httplib::request &req, httplib::response &resp) ->
    ////    boost::asio::awaitable<void> {
    ////        req.is_body_type<httplib::form_data_body>();

    ////        resp.base().result(httplib::http::status::ok);
    ////        resp.set_body<httplib::form_data_body>(req.body<httplib::form_data_body>());
    ////        co_return;
    ////    },
    ////    log_t{});
    // httplib::http::fields header;
    // header.set("Cross-Origin-Opener-Policy", "same-origin");
    // header.set("Cross-Origin-Embedder-Policy", "require-corp");
    // header.set("Access-Control-Allow-Origin", "*");
    // router.set_mount_point(
    //     "/",
    //     R"(E:/)",
    //     header);

    // router.set_mount_point("/files", R"(D:/)", header);
    svr.async_run();

    pool.wait();
}