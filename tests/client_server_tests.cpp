#include "httplib/body/json_body.hpp"
#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/html/query_params.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::string_view_literals;

struct test_failure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

#define HTTPLIB_TEST_REQUIRE(expr)                                                                 \
    do {                                                                                           \
        if (!(expr))                                                                               \
            throw test_failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +            \
                               ": requirement failed: " #expr);                                    \
    } while (false)

#define HTTPLIB_TEST_REQUIRE_EQ(lhs, rhs)                                                          \
    do {                                                                                           \
        const auto& lhs_value = (lhs);                                                             \
        const auto& rhs_value = (rhs);                                                             \
        if (!(lhs_value == rhs_value))                                                             \
            throw test_failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +            \
                               ": equality failed: " #lhs " == " #rhs);                           \
    } while (false)

std::string as_string(const httplib::client::http_client::response& resp)
{
    return resp.body().as<httplib::body::string_body>();
}

const boost::json::value& as_json(const httplib::client::http_client::response& resp)
{
    return resp.body().as<httplib::body::json_body>();
}

httplib::client::http_client::response
unwrap(httplib::client::http_client::response_result&& result)
{
    if (!result)
        throw test_failure("client request failed: " + result.error().message());
    return std::move(result).value();
}

httplib::html::query_params params(std::initializer_list<std::pair<std::string, std::string>> vals)
{
    httplib::html::query_params out;
    for (const auto& [key, val] : vals)
        out.add(key, val);
    return out;
}

httplib::http::fields headers(std::initializer_list<std::pair<httplib::http::field, std::string>> vals)
{
    httplib::http::fields out;
    for (const auto& [key, val] : vals)
        out.set(key, val);
    return out;
}

void set_text(httplib::server::response& resp,
              std::string_view body,
              httplib::http::status status = httplib::http::status::ok)
{
    resp.set_string_content(body, "text/plain"sv, status);
}

void expect_common_request(httplib::server::request& req,
                           httplib::http::verb method,
                           std::string_view expected_name)
{
    HTTPLIB_TEST_REQUIRE_EQ(req.method(), method);
    HTTPLIB_TEST_REQUIRE_EQ(req.path(), "/method/" + std::string(expected_name));
    HTTPLIB_TEST_REQUIRE_EQ(req.query_params().at("q"), "1");
    HTTPLIB_TEST_REQUIRE_EQ(req.base()[httplib::http::field::user_agent], "httplib-test");
}

void configure_routes(httplib::server::http_server& server)
{
    auto& router = server.router();

    router.set_http_handler<httplib::http::verb::get>(
        "/method/get",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::get, "get");
            set_text(resp, "get-ok"sv);
        });

    router.set_http_handler<httplib::http::verb::head>(
        "/method/head",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::head, "head");
            resp.set("X-Head-Test", "head-ok");
            set_text(resp, "head-body-is-not-sent"sv);
        });

    router.set_http_handler<httplib::http::verb::post>(
        "/method/post",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::post, "post");
            HTTPLIB_TEST_REQUIRE_EQ(req.body().as<httplib::body::string_body>(), "post-body");
            set_text(resp, "post-ok"sv);
        });

    router.set_http_handler<httplib::http::verb::post>(
        "/method/post-json",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::post, "post-json");
            const auto& obj = req.body().as<httplib::body::json_body>().as_object();
            HTTPLIB_TEST_REQUIRE_EQ(obj.at("name").as_string(), "client");
            resp.set_json_content({{"ok", true}, {"echo", obj.at("name")}});
        });

    router.set_http_handler<httplib::http::verb::put>(
        "/method/put-empty",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::put, "put-empty");
            set_text(resp, "put-empty-ok"sv);
        });

    router.set_http_handler<httplib::http::verb::put>(
        "/method/put",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::put, "put");
            HTTPLIB_TEST_REQUIRE_EQ(req.body().as<httplib::body::string_body>(), "put-body");
            set_text(resp, "put-ok"sv);
        });

    router.set_http_handler<httplib::http::verb::put>(
        "/method/put-json",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::put, "put-json");
            const auto& obj = req.body().as<httplib::body::json_body>().as_object();
            HTTPLIB_TEST_REQUIRE_EQ(obj.at("name").as_string(), "put-json");
            resp.set_json_content({{"ok", true}, {"method", "put"}});
        });

    router.set_http_handler<httplib::http::verb::patch>(
        "/method/patch",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::patch, "patch");
            HTTPLIB_TEST_REQUIRE_EQ(req.body().as<httplib::body::string_body>(), "patch-body");
            set_text(resp, "patch-ok"sv);
        });

    router.set_http_handler<httplib::http::verb::patch>(
        "/method/patch-json",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::patch, "patch-json");
            const auto& obj = req.body().as<httplib::body::json_body>().as_object();
            HTTPLIB_TEST_REQUIRE_EQ(obj.at("name").as_string(), "patch-json");
            resp.set_json_content({{"ok", true}, {"method", "patch"}});
        });

    router.set_http_handler<httplib::http::verb::delete_>(
        "/method/delete",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::delete_, "delete");
            set_text(resp, "delete-ok"sv);
        });

    router.set_http_handler<httplib::http::verb::options>(
        "/method/options",
        [](httplib::server::request& req, httplib::server::response& resp) {
            expect_common_request(req, httplib::http::verb::options, "options");
            resp.set("Allow", "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
            set_text(resp, "options-ok"sv);
        });

    router.set_http_handler<httplib::http::verb::post>(
        "/method/send-request",
        [](httplib::server::request& req, httplib::server::response& resp) {
            HTTPLIB_TEST_REQUIRE_EQ(req.method(), httplib::http::verb::post);
            HTTPLIB_TEST_REQUIRE_EQ(req.body().as<httplib::body::string_body>(), "generic-body");
            set_text(resp, "send-request-ok"sv);
        });

    router.set_http_handler<httplib::http::verb::get>(
        "/stream",
        [](httplib::server::request&, httplib::server::response& resp) {
            auto index = std::make_shared<int>(0);
            resp.set_stream_content(
                [index](httplib::beast::flat_buffer& buffer,
                        boost::system::error_code&) -> httplib::net::awaitable<bool> {
                    static constexpr std::string_view chunks[] = {"chunk-", "handler-", "ok"};
                    if (*index >= static_cast<int>(std::size(chunks)))
                        co_return false;

                    auto chunk = chunks[*index];
                    ++(*index);
                    buffer.commit(httplib::net::buffer_copy(buffer.prepare(chunk.size()),
                                                            httplib::net::buffer(chunk)));
                    co_return *index < static_cast<int>(std::size(chunks));
                },
                "text/plain");
        });

    router.set_http_handler<httplib::http::verb::get>(
        "/pool",
        [](httplib::server::request& req, httplib::server::response& resp) {
            HTTPLIB_TEST_REQUIRE_EQ(req.query_params().at("id"), "42");
            set_text(resp, "pool-ok"sv);
        });

    router.set_http_not_found_handler(
        [](httplib::server::request& req, httplib::server::response& resp) {
            HTTPLIB_TEST_REQUIRE_EQ(req.path(), "/missing");
            set_text(resp, "missing"sv, httplib::http::status::not_found);
        });
}

class test_server
{
public:
    test_server()
        : server_(ioc_)
    {
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        server_.set_logger(std::make_shared<spdlog::logger>("httplib.tests", null_sink));
        configure_routes(server_);
        server_.listen("127.0.0.1", 0);
        endpoint_ = server_.local_endpoint();
        server_.async_run();
        thread_ = std::thread([this] { ioc_.run(); });
    }

    ~test_server()
    {
        server_.stop();
        ioc_.stop();
        if (thread_.joinable())
            thread_.join();
    }

    httplib::net::any_io_executor executor() { return ioc_.get_executor(); }
    std::string host() const { return endpoint_.address().to_string(); }
    uint16_t port() const { return endpoint_.port(); }

private:
    httplib::net::io_context ioc_;
    httplib::server::http_server server_;
    httplib::tcp::endpoint endpoint_;
    std::thread thread_;
};

void run_client_server_tests()
{
    test_server server;
    httplib::client::http_client client(server.executor(), server.host(), server.port());
    client.set_timeout(std::chrono::seconds(5));

    auto base_headers = headers({{httplib::http::field::user_agent, "httplib-test"}});

    auto resp = unwrap(client.get("/method/get", params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(as_string(resp), "get-ok");

    resp = unwrap(client.head("/method/head", params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(resp[httplib::http::field::content_type], "text/plain");
    HTTPLIB_TEST_REQUIRE_EQ(resp["X-Head-Test"], "head-ok");

    resp =
        unwrap(client.post("/method/post", "post-body"sv, params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(as_string(resp), "post-ok");

    resp = unwrap(client.post(
        "/method/post-json", {{"name", "client"}}, params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE(as_json(resp).as_object().at("ok").as_bool());
    HTTPLIB_TEST_REQUIRE_EQ(as_json(resp).as_object().at("echo").as_string(), "client");

    resp = unwrap(client.put("/method/put-empty", params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(as_string(resp), "put-empty-ok");

    resp = unwrap(client.put("/method/put", "put-body"sv, params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(as_string(resp), "put-ok");

    resp = unwrap(client.put(
        "/method/put-json", {{"name", "put-json"}}, params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(as_json(resp).as_object().at("method").as_string(), "put");

    resp =
        unwrap(client.patch("/method/patch", "patch-body"sv, params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(as_string(resp), "patch-ok");

    resp = unwrap(client.patch(
        "/method/patch-json", {{"name", "patch-json"}}, params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(as_json(resp).as_object().at("method").as_string(), "patch");

    resp = unwrap(client.del("/method/delete", params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(as_string(resp), "delete-ok");

    resp = unwrap(client.options("/method/options", params({{"q", "1"}}), base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(resp[httplib::http::field::allow],
                            "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
    HTTPLIB_TEST_REQUIRE_EQ(as_string(resp), "options-ok");

    resp = unwrap(client.send_request(
        httplib::http::verb::post, "/method/send-request", "generic-body"sv, base_headers));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    HTTPLIB_TEST_REQUIRE_EQ(as_string(resp), "send-request-ok");

    std::string streamed;
    client.set_chunk_handler(
        [&streamed](std::string_view chunk, boost::system::error_code&) { streamed += chunk; });
    resp = unwrap(client.get("/stream"));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
    if (streamed != "chunk-handler-ok")
        std::cerr << "streamed body was: [" << streamed << "], response body: [" << as_string(resp)
                  << "]\n";
    HTTPLIB_TEST_REQUIRE_EQ(streamed, "chunk-handler-ok");
    client.set_chunk_handler(nullptr);

    resp = unwrap(client.get("/missing"));
    HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::not_found);
    HTTPLIB_TEST_REQUIRE_EQ(as_string(resp), "missing");

    httplib::client::http_client_pool pool(server.executor(), 2);
    {
        auto pooled = pool.acquire(server.host(), server.port());
        resp = unwrap(pooled->get("/pool", params({{"id", "42"}})));
        HTTPLIB_TEST_REQUIRE_EQ(resp.result(), httplib::http::status::ok);
        HTTPLIB_TEST_REQUIRE_EQ(as_string(resp), "pool-ok");
    }
}

} // namespace

int main()
{
    try {
        run_client_server_tests();
        std::cout << "httplib client/server tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& ex) {
        std::cerr << "httplib client/server tests failed: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
