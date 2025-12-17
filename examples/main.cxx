
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
#include <spdlog/spdlog.h>


using namespace std::string_view_literals;

constexpr auto server_crt = R"(-----BEGIN CERTIFICATE-----
MIIDKDCCAhACCQDHu0UVVUEr4DANBgkqhkiG9w0BAQsFADBWMQswCQYDVQQGEwJD
TjEVMBMGA1UEBwwMRGVmYXVsdCBDaXR5MRwwGgYDVQQKDBNEZWZhdWx0IENvbXBh
bnkgTHRkMRIwEAYDVQQDDAlsb2NhbGhvc3QwHhcNMjIxMDI1MDM1NzMwWhcNMzIx
MDIyMDM1NzMwWjBWMQswCQYDVQQGEwJDTjEVMBMGA1UEBwwMRGVmYXVsdCBDaXR5
MRwwGgYDVQQKDBNEZWZhdWx0IENvbXBhbnkgTHRkMRIwEAYDVQQDDAlsb2NhbGhv
c3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCr6iWgRRYJ9QfKSUPT
nbw2rKZRlSBqnLeLdPam+s8RUA1p+YPoH2HJqIdxcfYmToz5t6G5OX8TFhAssShw
PalRlQm5QHp4pL7nqPV79auB3PYKv6TgOumwDUpoBxcu0l9di9fjYbC2LmpVJeVz
WQxCo+XO/g5YjXN1nPPeBgmZVkRvXLIYCTKshLlUa0nW7hj7Sl8CAV8OBNMBFkf1
2vgcTqhs3yW9gnIwIoCFZvsdAsSbwR6zF1z96MeAYDIZWeyzUXkoZa4OCWwAhqzo
+0JWukuNuHhsQhIJDvIZWHEblT0GlentP8HPXjFnJHYGUAjx3Fj1mH8mFG0fEXXN
06qlAgMBAAEwDQYJKoZIhvcNAQELBQADggEBAGbKTy1mfSlJF012jKuIue2valI2
CKz8X619jmxxIzk0k7wcmAlUUrUSFIzdIddZj92wYbBC1YNOWQ4AG5zpFo3NAQaZ
kYGnlt+d2pNHLaT4IV9JM4iwTqPyi+FsOwTjUGHgaOr+tfK8fZmPbDmAE46OlC/a
VVqNPmjaJiM2c/pJOs+HV9PvEOFmV9p5Yjjz4eV3jwqHdOcxZuLJl28/oqz65uCu
LQiivkdVCuwc1IlpRFejkrbkrk28XCCJwokLt03EQj4xs0sjoTKgd92fpjls/tt+
rw+7ILsAsuoWPIdiuCArCU1LXJDz3FDHafX/dxzdVBzpfVgP0rNpS050Mls=
-----END CERTIFICATE-----
)"sv;


constexpr auto server_key = R"(-----BEGIN RSA PRIVATE KEY-----
Proc-Type: 4,ENCRYPTED
DEK-Info: DES-EDE3-CBC,D920B8941C56ADDC

I2lW3QsAG/xubjtXpXh3wQ5Ru3VZiMkPNjc+G6/2JjjVr1sD+fzCWvvwdqdxGuNJ
gKdpPBHLuQfTTzGETE4NKDkYzmiPTVbZPJ77DyfL2cK1dcZtAY46RsHf+VMI5N8l
Be1jQSB5xvUa88dSIeowPTc2XSnTIoSFWCa38XuqYF7i0a3lv96eAyXpqB7Tm2r8
SoYlm0n7/uzRpk6HWST65qnVv/j+37LuvSy6ehyh44+KDS4x9FUOZc5xwJ/37Jnl
SDC10+9zLc+jOTk6XgUuBSmG+xfZdcOrbknQ1Xj1YtseYH0plYAEWi4PsnMQkHzC
GGvK08Lgqxd7cGEKFh2MRZ/TEwriN5ud5HGm4yIHIj45rbedtRSQwl2EyHdWeW0J
rFltDy+SXnnkJaOcnBYXUD1jEwyy2lLamWRiu83VFbCv6yhOYuR6JejM6dctjgZ+
Qf0PzH6L1bVpHKEl/GLByJ6GWYrQJqw83LAXlR+NNCC3nN7WAAaTuzA9LpgW9Vk0
khRRs7rJGxwwwE4TfG9FbQxwuOsjKV9pRohB1x1nFMMm5IJ9SON2KjizsVdLbt7t
Gb/5M7RcSnnGvIWWXalXpFGKgciwYd8F1v0TJ+FMooZxgUp7Pmp5YKIHkBjMrnnW
rKuoxmA5oPgSNUtr4ddMJ1sTIQPhqI27+CrySTzWKH1ls45okBvsiCejpcJwfrZW
KLSkz/FsPoWm44uomBSDOikry8axrKQLB9tOVPKCx/z0VP060P9N81mu4h67bixr
xu+odIONqGhRZT/BYHL2NjDfWlFmTJQy8Drn1a7IEhp8FV7l2aY/hisrMN7MQVza
FGB0hMbVHGeFOCD9QNQwRU2wLtwpE7LT/lGNmKadQadXxeAqOWBckXrpwnrxZDEP
a8AYr2J55h/IE4Oi2DyibSEZdB+7334OJHMmr14q53eIpeit19BYVhWyu9AtORJp
As61C7s82AO+E5gOswsq05jwWV/GIIkgZ8/vswEffiihmDEf6AUZsVGW3BlpFlyU
i3g4e8HFTJ+s9Z3sTgZ1EWOP6Wd2OzyQYVA4ggBR/g/IC9s5em1wvAkVwIZaPvj7
21BIQXyiGrw52T+vTUrAUG0l7yoHGCgVYJ+aEm+f103AiBYuReUbo39GEIY2GHLu
r3oUehtt4of0ootmPCmjrRUyY6LPeD+d+i1jJUSYFKezsVRpaiF5+J8YLGMcOPiI
8qRRNgXDMMvttwyhoxyr5+667OMv+XWr2VQj7i9MWCFwTMwNzdUoZI3PWDhXbXDO
lQJS6v3iAPw+KvLJywODe+C4shUqYdrRdUSKE0FfuB8Ajzh86+FmjJcZM+BSxM4J
hC2yjv114jDlsgjFSxQE2K1iotLUY9mfmW8QWVMO3L4LlNpr4ypNLYX0Ph2wgqzQ
kszXTFN11RFKFLUhF0Mi5m4ffMLPD5YyoqO9grpyC1Nt7vxaPPvcvPD86jK3ksqJ
MwucZGgm9HtUuAjGOSljUr0d+d+4pySJbcpH2YDIBHGVsCScYPVg8XZ1CYko3mq/
d6jDUgydraEmQvIPiKMpTE18rW+jierv2FlB8AGcwxm2VWxuM25wQ40J2YuZLY7k
-----END RSA PRIVATE KEY-----
)"sv;

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
    httplib::net::awaitable<void> echo_json(httplib::server::request& req,
                                            httplib::server::response& resp)
    {
        const auto& req_json = req.body().as<httplib::body::json_body>();
        resp.set_json_content(req_json);
        co_return;
    }
};

int main(int argc, char** argv)
{
    boost::asio::thread_pool pool;
    httplib::server::http_server svr(pool.get_executor());
    svr.get_logger()->set_level(spdlog::level::info);
    svr.use_ssl(server_crt, server_key, "test");
    svr.listen("0.0.0.0", 18808);

    auto& router = svr.router();

    // http://127.0.0.1:18808/regex/10000
    router.set_http_handler<httplib::http::verb::get>(
        "/regex/{id:^\\d+$}",
        [](httplib::server::request& req, httplib::server::response& resp) {
            resp.set_string_content(req.path_param("id"), "text/plain");
            return;
        },
        log_t {});

    // http://127.0.0.1:18808/regex/10000.0/n1/n2
    router.set_http_handler<httplib::http::verb::get>(
        "/regex/*",
        [](httplib::server::request& req, httplib::server::response& resp) {
            resp.set_string_content(req.path_param("*"), "text/plain");
            return;
        },
        log_t {});

    router.set_http_handler<httplib::http::verb::get, httplib::http::verb::post>(
        "/path_param/:w",
        [](httplib::server::request& req, httplib::server::response& resp) {
            resp.set_string_content(req.path_param("w"), "text/plain");
            return;
        },
        log_t {});


    router.set_http_handler<httplib::http::verb::post>(
        "/x-www-from-urlencoded",
        [](httplib::server::request& req,
           httplib::server::response& resp) -> httplib::net::awaitable<void> {
            const auto& doc = req.body().as<httplib::body::query_params_body>();
            resp.set_string_content(doc.encoded(), "text/plain");
            co_return;
        },
        log_t {});

    router.set_http_post_handler([](httplib::server::request& req,
                                    httplib::server::response& resp) {
        resp.set("Access-Control-Allow-Origin", "*");
        resp.set("Access-Control-Allow-Credentials", "true");
        resp.set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        resp.set(
            "Access-Control-Allow-Headers",
            R"(Origin, Content-Type, Content-Length, Accept-Encoding, X-CSRF-Token, Authorization)");
    });
    router.set_http_handler<httplib::http::verb::options>(
        "/*", [](httplib::server::request& req, httplib::server::response& resp) {
            resp.set_empty_content(httplib::http::status::no_content);
        });

    router.set_ws_handler(
        "/ws",
        [&](httplib::server::websocket_conn::weak_ptr hdl) -> boost::asio::awaitable<void> {
            auto conn = hdl.lock();
            svr.get_logger()->info("new ws: [{}:{}]",
                                   conn->http_request().remote_endpoint().address().to_string(),
                                   conn->http_request().remote_endpoint().port());
            co_return;
        },
        [&](httplib::server::websocket_conn::weak_ptr hdl,
            std::string_view msg,
            bool binary) -> void {
            auto conn = hdl.lock();
            svr.get_logger()->info("ws msg: {}", msg);
            conn->send_message(msg, binary);
            return;
        },
        [&](httplib::server::websocket_conn::weak_ptr hdl) {
            auto conn = hdl.lock();
            svr.get_logger()->info("close ws: [{}:{}]",
                                   conn->http_request().remote_endpoint().address().to_string(),
                                   conn->http_request().remote_endpoint().port());
            return;
        });

    router.set_http_handler<httplib::http::verb::get>(
        "/stream",
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
                "text/plain");
            co_return;
        },
        log_t {});

    test tt;
    router.set_http_handler<httplib::http::verb::post>("/json", &test::echo_json, tt, log_t {});
    router.set_http_handler<httplib::http::verb::get>(
        "/close", [&](httplib::server::request& req, httplib::server::response& resp) {
            svr.stop();
            return;
        });

    router.set_static_mount_point("/files", "./", log_t {});
    svr.async_run();

    // Run the I/O service on the requested number of threads
    pool.wait();
}