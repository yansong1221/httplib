
#include "httplib/router.hpp"
#include "httplib/server.hpp"
#include "httplib/setting.hpp"

#include <filesystem>
#include <format>
#include <iostream>
// 日志切面
struct log_t
{
    httplib::net::awaitable<bool> before(httplib::request& req, httplib::response& res)
    {
        start_ = std::chrono::steady_clock::now();
        co_return true;
    }

    bool after(httplib::request& req, httplib::response& res)
    {
        auto span = std::chrono::steady_clock::now() - start_;
        std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(span).count()
                  << std::endl;
        return true;
    }

private:
    std::chrono::steady_clock::time_point start_;
};
int main()
{ // HTTP
    using namespace std::string_view_literals;
    httplib::server svr;

    auto& option = svr.option();
    auto& router = svr.router();

    auto config = httplib::server::setting::SSLConfig {
        R"(D:\code\http\lib\server.crt)", R"(D:\code\http\lib\server.key)", "test"};
    option.ssl_conf = config;
    option.get_logger()->set_level(spdlog::level::trace);

    svr.listen("127.0.0.1", 8808);
    option.websocket_open_handler =
        [](httplib::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> { co_return; };
    option.websocket_close_handler =
        [](httplib::websocket_conn::weak_ptr conn) -> boost::asio::awaitable<void> { co_return; };
    option.websocket_message_handler =
        [](httplib::websocket_conn::weak_ptr hdl,
           httplib::websocket_conn::message msg) -> boost::asio::awaitable<void> {
        auto conn = hdl.lock();
        conn->send_message(msg);
        co_return;
    };

    router.set_http_handler<httplib::http::verb::post, httplib::http::verb::get>(
        "/中文",
        [](httplib::request& req, httplib::response& resp) -> httplib::net::awaitable<void> {
            // req.is_body_type<httplib::body::form_data_body>();

            resp.base().result(httplib::http::status::ok);
            // resp.set_body<httplib::body::form_data_body>(req.body<httplib::body::form_data_body>());
            co_return;
        },
        log_t {});
    router.set_http_handler<httplib::http::verb::get>(
        "/close", [&](httplib::request& req, httplib::response& resp) { svr.stop(); });
    router.set_http_handler<httplib::http::verb::post>(
        "/json",
        [](httplib::request& req, httplib::response& resp) -> httplib::net::awaitable<void> {
            auto& doc = req.body().as<httplib::body::json_body>();

            /*           const auto &obj = doc.get_object();
            for (const auto &item : obj.at("statuses").as_array()) {
                std::string_view created_at = item.at("created_at").as_string();
                resp.set_string_content(std::string(created_at), "text/html");
                co_return;
            }*/
            resp.set_json_content(doc);
            co_return;
        },
        log_t {});

    router.set_http_handler<httplib::http::verb::post>(
        "/x-www-from-urlencoded",
        [](httplib::request& req, httplib::response& resp) -> httplib::net::awaitable<void> {
            auto& doc = req.body().as<httplib::body::query_params_body>();

            /*           const auto &obj = doc.get_object();
            for (const auto &item : obj.at("statuses").as_array()) {
                std::string_view created_at = item.at("created_at").as_string();
                resp.set_string_content(std::string(created_at), "text/html");
                co_return;
            }*/
            resp.set_error_content(httplib::http::status::internal_server_error);
            co_return;
        },
        log_t {});
    // router.set_default_handler(
    //     [](httplib::request& req, httplib::response& resp) ->
    //     httplib::net::awaitable<void>
    //     {
    //         httplib::client cli(co_await httplib::net::this_coro::executor,
    //         "127.0.0.1", 8888); cli.set_use_ssl(false);

    //        boost::json::object doc;
    //        doc["hello"] = "world";
    //        auto result = co_await cli.async_post("/json", doc);
    //        if (!result)
    //        {
    //            resp.set_error_content(httplib::http::status::bad_gateway);
    //            co_return;
    //        }
    //        resp = std::move(*result);
    //        // resp.set_string_content("hello"sv, "text/html");
    //        co_return;
    //    });
    // svr.set_http_handler<httplib::http::verb::post, httplib::http::verb::get>(
    //     "/hello/:w",
    //     [](httplib::request &req, httplib::response &resp) {
    //         req.is_body_type<httplib::body::form_data_body>();

    //        resp.base().result(httplib::http::status::ok);
    //        resp.set_string_content("1000", "text/html");
    //        return;
    //    },
    //    log_t{});
    // svr.set_http_handler<httplib::http::verb::post>(
    //    "/",
    //    [](httplib::request &req, httplib::response &resp) ->
    //    boost::asio::awaitable<void> {
    //        req.is_body_type<httplib::form_data_body>();

    //        resp.base().result(httplib::http::status::ok);
    //        resp.set_body<httplib::form_data_body>(req.body<httplib::form_data_body>());
    //        co_return;
    //    },
    //    log_t{});
    httplib::http::fields header;
    header.set("Cross-Origin-Opener-Policy", "same-origin");
    header.set("Cross-Origin-Embedder-Policy", "require-corp");
    header.set("Access-Control-Allow-Origin", "*");
    router.set_mount_point(
        "/",
        R"(F:/Qt/Examples/Qt-6.8.2/demos/mediaplayer/build/WebAssembly_Qt_6_8_2_multi_threaded-Debug)",
        header);

    router.set_mount_point("/files", R"(D:/)", header);

    svr.run();
}