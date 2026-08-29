#include "httplib/client/client.hpp"
#include "httplib/client/client_pool.hpp"
#include "httplib/client/lazy_request.hpp"
#include "httplib/client/stream_reader.hpp"
#include "httplib/client/ws_client.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/html/query_params.hpp"
#include "httplib/server/chunk_reader.hpp"
#include "httplib/server/chunk_writer.hpp"
#include "httplib/server/middleware/auth.hpp"
#include "httplib/server/middleware/cors.hpp"
#include "httplib/server/middleware/rate_limit.hpp"
#include "httplib/server/mount_point_entry.hpp"
#include "httplib/server/ndjson_writer.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include "httplib/server/sse_writer.hpp"
#include "httplib/version.hpp"
#include <array>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/json.hpp>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <spdlog/spdlog.h>

using namespace std::string_view_literals;
namespace fs = std::filesystem;
namespace beast = httplib::beast;
namespace http = httplib::http;
namespace net = httplib::net;
namespace mw = httplib::server::middleware;

#ifdef HTTPLIB_ENABLED_SSL
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
#endif

// ===== Custom aspect example =====
// Users can write their own aspect classes for custom middleware needs.
// An aspect needs a before() and/or after() method taking (request&, response&).

struct log_t
{
    bool
    before(httplib::server::request& req, httplib::server::response&)
    {
        spdlog::info("[{}] {}", req.method_string(), req.path());
        return true;
    }
    bool
    after(httplib::server::request& req, httplib::server::response&)
    {
        spdlog::info("[{}] {} -> done", req.method_string(), req.path());
        return true;
    }
};

// ===== Server setup =====

static void
setup_http_routes(httplib::server::router& router)
{
    // ---- Built-in middleware: cors_middleware (applied per-route as an aspect) ----
    // cors_middleware can also be set globally via the post_handler pattern (see setup_global_cors below)

    // ---- Basic HTTP methods (with cors_middleware) ----
    router.set_http_handler<http::verb::get>(
        "/api/hello",
        [](httplib::server::request&, httplib::server::response& resp)
        { resp.set_string_content("Hello, World!"sv, "text/plain"sv); },
        mw::cors_middleware {});

    router.set_http_handler<http::verb::get>("/api/greet/:name",
                                             [](httplib::server::request& req, httplib::server::response& resp)
                                             {
                                                 auto name = std::string(req.path_param("name"));
                                                 resp.set_string_content(std::format("Hello, {}!", name),
                                                                         "text/plain"sv);
                                             });

    // ---- JSON echo ----
    router.set_http_handler<http::verb::post>("/api/echo-json",
                                              [](httplib::server::request& req, httplib::server::response& resp)
                                              { resp.set_json_content(req.as_json()); });

    // ---- URL-encoded form ----
    router.set_http_handler<http::verb::post>("/api/form-urlencoded",
                                              [](httplib::server::request& req, httplib::server::response& resp)
                                              {
                                                  auto const& params = req.as_query_params();
                                                  boost::json::object obj;
                                                  for (auto const& [k, v] : params.params())
                                                  {
                                                      obj[k] = v;
                                                  }
                                                  resp.set_json_content(std::move(obj));
                                              });

    // ---- Multipart form data ----
    router.set_http_handler<http::verb::post>("/api/form-multipart",
                                              [](httplib::server::request& req, httplib::server::response& resp)
                                              {
                                                  auto const& fd = req.as_form_data();
                                                  boost::json::object obj;
                                                  for (auto const& field : fd.fields)
                                                  {
                                                      boost::json::object f;
                                                      f["name"] = field.name;
                                                      f["size"] = static_cast<int64_t>(field.content.size());
                                                      f["is_file"] = field.is_file();
                                                      if (!field.filename.empty())
                                                      {
                                                          f["filename"] = field.filename;
                                                      }
                                                      obj[field.name] = std::move(f);
                                                  }
                                                  resp.set_json_content(std::move(obj));
                                              });

    // ---- RESTful: PUT / PATCH / DELETE ----
    router.set_http_handler<http::verb::put>("/api/resource/:id",
                                             [](httplib::server::request& req, httplib::server::response& resp)
                                             {
                                                 auto id = std::string(req.path_param("id"));
                                                 resp.set_json_content({
                                                     { "updated", id }
                                                 });
                                             });

    router.set_http_handler<http::verb::patch>("/api/resource/:id",
                                               [](httplib::server::request& req, httplib::server::response& resp)
                                               {
                                                   auto id = std::string(req.path_param("id"));
                                                   resp.set_json_content({
                                                       { "patched", id }
                                                   });
                                               });

    router.set_http_handler<http::verb::delete_>("/api/resource/:id",
                                                 [](httplib::server::request& req, httplib::server::response& resp)
                                                 {
                                                     resp.set_json_content({
                                                         { "deleted", std::string(req.path_param("id")) }
                                                     });
                                                 });

    // ---- OPTIONS (cors_middleware preflight handled by cors_middleware) ----
    router.set_http_handler<http::verb::options>("/*",
                                                 [](httplib::server::request&, httplib::server::response& resp)
                                                 {
                                                     resp.set(http::field::allow,
                                                              "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
                                                     resp.set_empty_content(http::status::no_content);
                                                 });

    // ---- Regex path param ----
    router.set_http_handler<http::verb::get>("/api/regex/{id:^\\d+$}",
                                             [](httplib::server::request& req, httplib::server::response& resp)
                                             {
                                                 resp.set_json_content({
                                                     { "id", std::string(req.path_param("id")) }
                                                 });
                                             });

    // ---- Wildcard ----
    router.set_http_handler<http::verb::get>("/api/files/*",
                                             [](httplib::server::request& req, httplib::server::response& resp)
                                             {
                                                 resp.set_json_content({
                                                     { "path", std::string(req.path_param("*")) }
                                                 });
                                             });

    // ---- Redirect ----
    router.set_http_handler<http::verb::get>("/api/redirect",
                                             [](httplib::server::request&, httplib::server::response& resp)
                                             { resp.set_redirect("/api/hello", http::status::moved_permanently); });

    // ---- Chunked streaming ----
    router.set_http_handler<http::verb::get>(
        "/api/stream",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto cw = resp.get_chunk_writer();
            http::fields headers;
            headers.set(http::field::content_type, "text/plain");
            co_await cw->write_header(http::status::ok, headers, false);
            for (int i = 0; i < 5; ++i)
            {
                co_await cw->write_body(net::buffer(std::format("chunk #{}\n", i)), i < 4);
            }
        });

    // ---- SSE (Server-Sent Events) ----
    router.set_http_handler<http::verb::get>(
        "/api/sse",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto sse = resp.create_sse_writer();
            co_await sse->begin();
            for (int i = 1; i <= 3; ++i)
            {
                co_await sse->send_event(std::format("event #{}", i), "tick", std::to_string(i), i == 3);
            }
        });

    // ---- NDJSON (Newline Delimited JSON) ----
    router.set_http_handler<http::verb::get>(
        "/api/ndjson",
        [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
        {
            auto w = resp.create_ndjson_writer();
            co_await w->begin();
            for (int i = 1; i <= 5; ++i)
            {
                co_await w->write(
                    {
                        { "seq",       i },
                        { "msg", "hello" }
                },
                    i == 5);
            }
        });

    router.set_chunked_http_handler<http::verb::post>(
        "/api/buffer",
        [](httplib::server::request& req, httplib::server::response& resp) -> httplib::net::awaitable<void>
        {
            std::string all;
            std::array<char, 1024> buffer;
            for (;;)
            {
                auto bytes_result = co_await req.get_chunk_reader()->read_some(httplib::net::buffer(buffer));
                if (bytes_result.has_error() || bytes_result.value() == 0)
                {
                    break;
                }
                all.append(buffer.data(), bytes_result.value());
            }
            resp.set_string_content(all, "text/plain");
            co_return;
        });

    // ---- Built-in middleware: Basic Auth ----
    router.set_http_handler<http::verb::get>(
        "/api/admin",
        [](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set_json_content({
                { "secret", "admin data" }
            });
        },
        mw::basic_auth_middleware([](std::string_view user, std::string_view pass)
                                  { return user == "admin" && pass == "secret"; },
                                  "Admin Area"));

    // ---- Built-in middleware: Bearer Token Auth ----
    router.set_http_handler<http::verb::get>(
        "/api/token-protected",
        [](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set_json_content({
                { "data", "token-gated content" }
            });
        },
        mw::bearer_auth_middleware([](std::string_view token) { return token == "my-secret-token"; }));

    // ---- Built-in middleware: Rate Limit (10 req / 10 seconds per IP) ----
    auto limiter = std::make_shared<mw::rate_limit_middleware>(10, std::chrono::seconds(10));
    router.set_http_handler<http::verb::get>(
        "/api/limited",
        [](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set_json_content({
                { "message", "you are not rate-limited... yet" }
            });
        },
        *limiter);

    // ---- Custom middleware: attach pre-computed data ----
    struct early_data_tag
    {
        std::string value;
    };
    class early_data_middleware
    {
      public:
        bool
        before(httplib::server::request& req, httplib::server::response&) const
        {
            req.data().store(early_data_tag { "computed-early" });
            return true;
        }
    };

    router.set_http_handler<http::verb::get>(
        "/api/custom-data",
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            auto& tag = req.data().fetch<early_data_tag>();
            resp.set_json_content({
                { "data", tag.value }
            });
        },
        early_data_middleware {});

    // ---- 404 handler with log aspect ----
    router.set_http_not_found_handler(
        [](httplib::server::request& req, httplib::server::response& resp)
        {
            resp.set_json_content(
                {
                    { "error",             "not found" },
                    {  "path", std::string(req.path()) }
            },
                http::status::not_found);
        },
        log_t {});
}

static void
setup_global_cors(httplib::server::router& router)
{
    // Global cors_middleware via post_handler (runs after every route handler)
    router.set_post_routing_handler(
        [](httplib::server::request&, httplib::server::response& resp)
        {
            resp.set(std::string_view("Access-Control-Allow-Origin"), std::string_view("*"));
            resp.set(std::string_view("Access-Control-Allow-Methods"),
                     std::string_view("GET, POST, PUT, DELETE, OPTIONS"));
            resp.set(std::string_view("Access-Control-Allow-Headers"),
                     std::string_view("Origin, Content-Type, Authorization"));
        });
}

static void
setup_ws(httplib::server::router& router)
{
    router.set_ws_handler(
        "/ws",
        [](httplib::server::websocket_conn::weak_ptr hdl) -> net::awaitable<void>
        {
            auto conn = hdl.lock();
            if (conn)
            {
                conn->send("Welcome!"sv, false);
            }
            co_return;
        },
        [](httplib::server::websocket_conn::weak_ptr hdl, std::string_view msg, bool binary) -> net::awaitable<void>
        {
            auto conn = hdl.lock();
            if (conn)
            {
                spdlog::info("WS received: {} (binary={})", msg, binary);
                conn->send(std::format("Echo: {}", msg), binary);
            }
            co_return;
        },
        [](httplib::server::websocket_conn::weak_ptr) -> net::awaitable<void>
        {
            spdlog::info("WS closed");
            co_return;
        });
}

static void
setup_static_files(httplib::server::router& router)
{
    auto mp = httplib::server::mount_point_entry("/static", fs::current_path());
    mp.set_directory_format(httplib::server::mount_point_entry::dir_format_type::html);
    mp.set_enabled_directory(true);
    router.set_static_mount_point(std::move(mp));
}

// ===== Client demo =====

static net::awaitable<void>
run_http_client_demo(net::any_io_executor ex, std::string host, uint16_t port)
{
    httplib::client::http_client client(ex, host, port);
    client.set_timeout(std::chrono::seconds(5));

    // GET
    {
        auto r = co_await client.async_get("/api/greet/client");
        if (r)
        {
            spdlog::info("GET  /api/greet/client -> {} [{}]", r.value().result_int(), r->as_string());
        }
    }

    // POST JSON
    {
        auto r = co_await client.async_post("/api/echo-json",
                                            boost::json::value {
                                                {   "msg", "hello" },
                                                { "count",      42 }
        });
        if (r)
        {
            spdlog::info("POST /api/echo-json -> {}", r.value().result_int());
        }
    }

    // DELETE
    {
        auto r = co_await client.async_del("/api/resource/99");
        if (r)
        {
            spdlog::info("DELETE /api/resource/99 -> {}", r.value().result_int());
        }
    }

    // OPTIONS
    {
        auto r = co_await client.async_options("/api/hello");
        if (r)
        {
            spdlog::info("OPTIONS /api/hello -> {} Allow={}",
                         r.value().result_int(),
                         std::string(r.value()[http::field::allow]));
        }
    }

    // Redirect
    {
        auto r = co_await client.async_get("/api/redirect");
        if (r)
        {
            spdlog::info("GET /api/redirect -> {} Location={}",
                         r.value().result_int(),
                         std::string(r.value()[http::field::location]));
        }
    }

    // Stream with chunk handler
    {
        auto resp = co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/api/stream"));
        if (resp)
        {
            while (true)
            {
                std::array<char, 4096> buf;
                auto result = co_await resp->read_some_raw(net::buffer(buf));
                if (result.has_error() || result.value() == 0)
                {
                    break;
                }
                spdlog::info("  chunk: {}", std::string_view(buf.data(), result.value()).substr(0, result.value() - 1));
            }
            spdlog::info("GET /api/stream -> {}", static_cast<unsigned>(resp->result()));
        }
    }

    // SSE (Server-Sent Events)
    {
        auto resp = co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/api/sse"));
        if (resp)
        {
            auto sse = resp->create_sse_reader();
            while (!sse->is_done())
            {
                auto result = co_await sse->read_event();
                if (result.has_error())
                {
                    break;
                }
                auto& ev = result.value();
                if (ev.data.empty() && ev.event.empty() && ev.id.empty() && ev.retry == std::chrono::milliseconds { 0 })
                {
                    break;
                }
                spdlog::info("  SSE event: id={} event={} data={}", ev.id, ev.event, ev.data);
            }
            spdlog::info("GET /api/sse -> ok");
        }
    }

    // NDJSON (Newline Delimited JSON)
    {
        auto resp = co_await client.async_send_request_lazy(httplib::client::request(http::verb::get, "/api/ndjson"));
        if (resp)
        {
            auto ndjson = resp->create_ndjson_reader();
            while (!ndjson->is_done())
            {
                auto result = co_await ndjson->read();
                if (result.has_error())
                {
                    break;
                }
                auto& val = result.value();
                if (val.is_null())
                {
                    break;
                }
                spdlog::info("  NDJSON line: {}", boost::json::serialize(val));
            }
            spdlog::info("GET /api/ndjson -> ok");
        }
    }

    // Auth: Basic (should fail without credentials)
    {
        auto r = co_await client.async_get("/api/admin");
        if (r)
        {
            spdlog::info("GET /api/admin (no auth) -> {}", r.value().result_int());
        }
    }

    // Auth: Basic (with correct credentials)
    {
        auto hdrs = httplib::http::fields();
        hdrs.set(http::field::authorization, "Basic YWRtaW46c2VjcmV0");
        auto r = co_await client.async_send_request(httplib::client::request(http::verb::get, "/api/admin", hdrs));
        if (r)
        {
            spdlog::info("GET /api/admin (with auth) -> {}", r.value().result_int());
        }
    }

    // 404
    {
        auto r = co_await client.async_get("/api/nonexistent");
        if (r)
        {
            spdlog::info("GET /api/nonexistent -> {}", r.value().result_int());
        }
    }
}

static net::awaitable<void>
run_http_client_pool_demo(net::any_io_executor ex, std::string host, uint16_t port)
{
    httplib::client::http_client_pool pool(ex, { .max_size = 4 });
    {
        auto h = co_await pool.async_acquire(host, port, false);
        if (h)
        {
            auto r = co_await h->async_get("/api/hello");
            if (r)
            {
                spdlog::info("Pool GET /api/hello -> {}", r.value().result_int());
            }
        }
    }
}

static void
run_ws_client_demo(net::any_io_executor ex, std::string host, uint16_t port)
{
    httplib::client::ws_client ws(ex, host, port);
    ws.run(
        "/ws",
        [&](boost::system::error_code ec) -> net::awaitable<void>
        {
            if (!ec)
            {
                spdlog::info("WS client connected");
                ws.send("Hello from WS client");
            }
            else
            {
                spdlog::error("WS connect error: {}", ec.message());
            }
            co_return;
        },
        [&](std::string_view msg, bool) -> net::awaitable<void>
        {
            spdlog::info("WS client received: {}", msg);
            ws.close();
            co_return;
        },
        []() -> net::awaitable<void>
        {
            spdlog::info("WS client disconnected");
            co_return;
        });
}

// ===== Main =====

static void
print_usage()
{
    std::cout << R"(httplib Demo v)" << httplib::version() << R"(

Usage: examples [mode] [options]

Modes:
  server       Start HTTP + WebSocket server (default)
  client       Run HTTP client demo against 127.0.0.1:18808
  ws           Run WebSocket client demo against 127.0.0.1:18808
  all          Start server then run all client demos

Server options:
  --port N     Server port (default: 18808)
  --no-ssl     Disable SSL/TLS

Client options:
  --host H     Server host (default: 127.0.0.1)
  --port N     Server port (default: 18808)

Built-in middleware on display:
  cors_middleware      cors_middleware header injection + OPTIONS preflight
  basic_auth_middleware      HTTP Basic auth (admin:secret on /api/admin)
  bearer_auth_middleware     Token auth (my-secret-token on /api/token-protected)
  rate_limit_middleware      10 req / 10 s per IP (on /api/limited)

Custom aspects:
  log_t                Request/response logging
   Custom middleware   Attaches computed data via req.data().store()

Press Ctrl+C to stop the server.
)";
}

int
main(int argc, char** argv)
{
    std::string mode = "server";
    std::string host = "0.0.0.0";
    uint16_t port = 18808;
    bool use_ssl = true;

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        if (arg == "server" || arg == "client" || arg == "ws" || arg == "all")
        {
            mode = arg;
        }
        else if (arg == "--port" && i + 1 < argc)
        {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if (arg == "--host" && i + 1 < argc)
        {
            host = argv[++i];
        }
        else if (arg == "--no-ssl")
        {
            use_ssl = false;
        }
        else if (arg == "--help" || arg == "-h")
        {
            print_usage();
            return 0;
        }
    }

    spdlog::set_level(spdlog::level::info);
    spdlog::info("httplib demo v{} | mode={} | host={} | port={} | ssl={}",
                 httplib::version(),
                 mode,
                 host,
                 port,
                 use_ssl);

    boost::asio::thread_pool pool(std::thread::hardware_concurrency());
    auto ex = pool.get_executor();

    if (mode == "client")
    {
        boost::asio::co_spawn(ex, run_http_client_demo(ex, host, port), boost::asio::use_future).get();
        boost::asio::co_spawn(ex, run_http_client_pool_demo(ex, host, port), boost::asio::use_future).get();
    }
    else if (mode == "ws")
    {
        run_ws_client_demo(ex, host, port);
    }
    else
    {
        httplib::server::http_server svr(ex);
        svr.logger()->set_level(spdlog::level::debug);

#ifdef HTTPLIB_ENABLED_SSL
        if (use_ssl)
        {
            svr.set_ssl(server_crt, server_key, "test");
        }
#else
        (void)use_ssl;
#endif
        svr.listen(host, port);

        auto& router = svr.router();

        // Enable compression only for compressible content types
        // Default: text/*, application/json, application/javascript, application/xml, image/svg+xml
        // Customize with a predicate:
        // svr.set_compress_content_types([](std::string_view ct) {
        //     return ct.starts_with("application/json");
        // });
        // To disable all compression: svr.set_compress_content_types([](std::string_view) { return
        // false; });

        setup_http_routes(router);
        setup_global_cors(router);
        setup_ws(router);
        setup_static_files(router);

        svr.set_reverse_proxy("/proxy", "http://127.0.0.1:18080");
        svr.set_ws_forward("/ws/forward", "ws://127.0.0.1:18080/ws");

        router.set_http_handler<http::verb::post>("/api/shutdown",
                                                  [&](httplib::server::request&, httplib::server::response& resp)
                                                  {
                                                      resp.set_json_content({
                                                          { "message", "shutting down" }
                                                      });
                                                      svr.stop();
                                                  });

        spdlog::info("Server listening on {}:{}", host, port);
        svr.run();

        if (mode == "all")
        {
            boost::asio::co_spawn(ex, run_http_client_demo(ex, host, port), boost::asio::use_future).get();
            boost::asio::co_spawn(ex, run_http_client_pool_demo(ex, host, port), boost::asio::use_future).get();
        }

        pool.wait();
    }

    return 0;
}
