# httplib

[English](README.en.md) | 简体中文

一个面向 C++23 的小巧、易嵌入的 HTTP/1.1 与 WebSocket 服务端 / 客户端库，构建于 [Boost.Beast](https://boost.org) 与 [Boost.Asio](https://boost.org) 之上。

版本：见 `httplib::version()`（`<httplib/version.hpp>`）。

## 特性

- **完整的 HTTP/1.1 支持** - GET、HEAD、POST、PUT、PATCH、DELETE、OPTIONS、CONNECT、TRACE
- **协程优先** - 所有 I/O 均为 `boost::asio::awaitable`；请求处理器可以是普通可调用对象，也可以是协程
- **异步客户端** - `async_*` 系列方法，外加连接池、惰性流式请求与断点下载
- **WebSocket** - 服务端与客户端，支持文本 / 二进制消息、ping/pong、断开回调
- **WebSocket 转发** - 反向代理实时 WebSocket 会话，支持拦截器
- **灵活路由** - 固定路径、`:named` 参数、`{param:regex}` 约束、`*` 通配符、成员函数处理器、多动词注册
- **多种 body 类型** - 字符串、JSON（Boost.JSON）、multipart 表单、URL 编码表单、文件 body、空 body
- **流式 body** - `set_lazy_http_handler` + `request::read_some_raw()` / `read_string()` / `read_json()` 流式读请求；`stream_writer` 流式写响应
- **SSE（Server-Sent Events）** - 服务端 `create_sse_writer()` / 客户端 `create_sse_reader()`
- **NDJSON** - 服务端 `create_ndjson_writer()` / 客户端 `create_ndjson_reader()`
- **重定向** - `resp.set_redirect(url, status)`，客户端可自动跟随
- **压缩** - Brotli content-encoding（可选，`-DHTTPLIB_ENABLED_COMPRESS=ON`）
- **SSL/TLS** - 通过 OpenSSL 支持 HTTPS 与 WSS（可选，`-DHTTPLIB_ENABLED_SSL=ON`）
- **JWT** - HS256/HS384/HS512 签名、校验、builder API，错误以 `boost::system::result` 返回
- **内置中间件** - CORS、Basic Auth、Bearer Auth、JWT Auth、限流、Session（基于 Cookie）、数据库访问 / 查询日志
- **全局中间件** - `router::use()` 对所有路由生效；`set_post_routing_handler()` 在每个处理器之后运行
- **自定义中间件** - 每路由的 `before`/`after` aspect，支持同步（`bool`）或协程（`awaitable<bool>`）返回
- **反向代理** - 静态或动态上游、加权 / 最少连接负载均衡、Cookie/Referer 改写、`X-Forwarded-*` 头、请求/响应拦截器
- **客户端下载栈** - `downloader`（多分段、断点续传、进度）、`download_scheduler`（并发上限）、`disk_cache`（按大小 / TTL 淘汰）
- **数据库** - 后端无关的 `session`，支持参数绑定、预编译语句、事务与连接池；SQLite / MySQL / ODBC 后端（可选，`-DHTTPLIB_ENABLED_DATABASE=ON`）
- **URL 工具** - 解析、解析相对路径、百分号编码、Host/URL 拼装
- **日志** - 集成 spdlog，各组件日志器可配置

## 平台支持

| 平台 | 编译器 |
|----------|----------|
| Windows  | MSVC, MinGW |
| Linux    | GCC, Clang |
| macOS    | GCC, Clang |
| FreeBSD  | Clang |

## 依赖

| 库 | 是否必需 | 说明 |
|---------|----------|-------|
| Boost（asio, beast, json, url） | 必需 | HTTP/WebSocket、JSON、URL 解析 |
| spdlog | 必需 | 日志 |
| fmt | 必需 | 字符串格式化 |
| OpenSSL | 可选 | SSL/TLS（`HTTPLIB_ENABLED_SSL`） |
| Boost.Iostreams + Brotli | 可选 | 压缩（`HTTPLIB_ENABLED_COMPRESS`） |
| Boost.MySQL (1.85+), Boost.Charconv | 可选 | MySQL 后端（`HTTPLIB_ENABLED_DATABASE`） |
| SQLite3 | 可选 | SQLite 后端（`HTTPLIB_ENABLED_DATABASE`） |
| ODBC（unixODBC / `odbc32`） | 可选 | ODBC 后端（`HTTPLIB_ENABLED_DATABASE`） |
| Catch2, jwt-cpp | 仅测试 | 测试套件 |

## 快速开始

```bash
mkdir build && cd build
cmake .. -DHTTPLIB_ENABLED_TESTS=ON
cmake --build .
```

构建产物位于 `bin/x64/[Debug|Release]/`（32 位为 `bin/x86/`）。

### 测试

```bash
ctest --test-dir build -C Debug          # 全部测试
ctest --test-dir build -C Debug -L http  # 按 label 过滤
```

可用 label：`core`、`http`、`client`、`proxy`、`jwt`、`db`。

### CMake 选项

| 选项 | 默认值 | 说明 |
|--------|---------|-------------|
| `HTTPLIB_ENABLED_SSL` | OFF | 启用 HTTPS/WSS（OpenSSL） |
| `HTTPLIB_ENABLED_COMPRESS` | OFF | 启用 Brotli 压缩 |
| `HTTPLIB_ENABLED_DATABASE` | OFF | 启用数据库模块（Boost.MySQL 1.85+、SQLite3、ODBC） |
| `HTTPLIB_ENABLED_EXAMPLES` | ON（根项目） | 构建示例 |
| `HTTPLIB_ENABLED_TESTS` | ON（根项目） | 构建测试套件 |
| `HTTPLIB_SHARED_LIBRARY` | OFF | 构建为动态库 |
| `HTTPLIB_ENABLED_UNITY_BUILD` | OFF | 启用 unity/jumbo 构建（测试仍保持 TU 隔离） |

## 用法

### 服务端

```cpp
#include <httplib/server/server.hpp>
#include <httplib/server/router.hpp>
#include <httplib/server/response.hpp>
#include <boost/asio/thread_pool.hpp>

using namespace httplib;

int main() {
    net::thread_pool pool(4);
    server::http_server svr(pool.get_executor());

    svr.router().set_http_handler<method::get>(
        "/api/hello",
        [](server::request&, server::response& resp) {
            resp.set_string_content("Hello, World!", "text/plain");
        });

    svr.listen("127.0.0.1", 8080);
    svr.run();        // 返回 future；也可用 async_run(ec)
    pool.join();
}
```

### 客户端

客户端是全异步的；每个请求返回 `net::awaitable<boost::system::result<client::response>>`。

```cpp
#include <httplib/client/client.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <spdlog/spdlog.h>

using namespace httplib;

net::awaitable<void> run(net::any_io_executor ex) {
    client::http_client client(ex, "127.0.0.1", 8080);   // 或 url::scheme::tls
    client.set_timeout(std::chrono::seconds(5));

    auto resp = co_await client.async_get("/api/hello");
    if (resp) {
        spdlog::info("{} {}", resp->result_int(), resp->as_string());
    }
}
```

### 基于 URL 的客户端

```cpp
client::http_client client(ex, "http://127.0.0.1:8080");
client::http_client secure(ex, "https://example.com");   // 由 scheme 推断 TLS
```

### 客户端连接池

```cpp
#include <httplib/client/client_pool.hpp>

client::http_client_pool pool(ex, { .max_size = 4 });   // 每个 host 4 条连接

net::co_spawn(ex, [&]() -> net::awaitable<void> {
    auto handle = co_await pool.async_acquire("127.0.0.1", 8080, url::scheme::plain);
    if (handle) {
        auto resp = co_await handle->async_get("/api/hello");
    }
    co_return;   // handle 析构时归还连接
}, net::detached);
```

`async_acquire` 也可直接接收完整 URL：`co_await pool.async_acquire("http://host:8080")`。

## 路由

```cpp
auto& router = svr.router();

// 固定路径
router.set_http_handler<method::get>("/api/hello", handler);

// 一次注册多个动词
router.set_http_handler<method::get, method::head>("/api/thing", handler);

// 命名参数（默认返回 string_view；其他类型用 path_param<int>("id")）
router.set_http_handler<method::get>(
    "/api/user/:id", [](server::request& req, server::response& resp) {
        auto id = req.path_param("id");
    });

// 正则约束
router.set_http_handler<method::get>("/api/item/{id:^\\d+$}", handler);

// 通配符
router.set_http_handler<method::get>(
    "/api/files/*", [](server::request& req, server::response& resp) {
        auto path = req.path_param("*");
    });

// 成员函数处理器
router.set_http_handler<method::get>("/api/member", &MyClass::handler, my_instance);

// CONNECT 与其它动词一样走普通路由
router.set_connect_handler("/tunnel", handler);

// 惰性处理器：请求体按需流式读取，不预先物化
router.set_lazy_http_handler<method::post>("/api/buffer", handler);

// 在每个路由处理器之后运行（例如注入全局响应头）
router.set_post_routing_handler([](server::request&, server::response& resp) {
    resp.set(httplib::field::server, "httplib");
});

// 自定义 404
router.set_http_not_found_handler([](server::request& req, server::response& resp) {
    resp.set_string_content("Not found", "text/plain", httplib::status::not_found);
});
```

注意：公共 API 使用库自身的枚举 `httplib::method` 与 `httplib::status`（并非 `boost::beast::http`），头字段使用 `httplib::headers` / `httplib::field`，因此 Boost 类型不会泄漏到用户代码中。

## 响应类型

```cpp
// 纯文本
resp.set_string_content("Hello", "text/plain");

// JSON
resp.set_json_content({{"ok", true}, {"data", 42}});

// 空响应（204 No Content）
resp.set_empty_content(httplib::status::no_content);

// 错误（默认 HTML body）
resp.set_error_content(httplib::status::internal_server_error);

// 重定向（status 默认 moved_permanently）
resp.set_redirect("/new-location", httplib::status::moved_permanently);

// 文件发送（支持 Range / ETag / If-Modified-Since）
resp.set_file_content("/path/to/file.pdf", req.base());

// multipart 表单
resp.set_form_data_content({
    form_data::field{ .name = "name", .content_type = "text/plain", .content = "value" }
});
```

### Chunked 流式响应

```cpp
router.set_http_handler<method::get>(
    "/stream",
    [](server::request&, server::response& resp) -> net::awaitable<void> {
        auto* writer = resp.create_stream_writer();
        httplib::headers headers;
        headers.set(httplib::field::content_type, "text/plain");
        co_await writer->write_header(httplib::status::ok, headers,
                                      server::stream_writer::mode::chunked);
        for (int i = 0; i < 5; ++i) {
            co_await writer->write_body(net::buffer(std::format("chunk #{}\n", i)), i < 4);
        }
    });
```

### SSE（Server-Sent Events）

```cpp
router.set_http_handler<method::get>(
    "/api/sse", [](server::request&, server::response& resp) -> net::awaitable<void> {
        auto sse = resp.create_sse_writer();
        co_await sse->begin();
        for (int i = 1; i <= 3; ++i) {
            co_await sse->send_event(std::format("event #{}", i), "tick",
                                     std::to_string(i), /*more=*/i < 3);
        }
    });
```

`create_sse_writer()` 返回 `unique_ptr<sse_writer>`。方法：`begin()`、`send_event(data, event, id, more)`、`send_retry(ms, more)`、`send_comment(msg, more)`。每个方法都有带 `error_code&` 的重载。

### NDJSON（Newline Delimited JSON）

```cpp
router.set_http_handler<method::get>(
    "/api/ndjson", [](server::request&, server::response& resp) -> net::awaitable<void> {
        auto w = resp.create_ndjson_writer();
        co_await w->begin();
        co_await w->write({{"seq", 1}, {"msg", "hello"}}, /*more=*/true);
        co_await w->write({{"seq", 2}, {"msg", "world"}}, /*more=*/false);
    });
```

## Body 类型

请求体被物化为类型安全的 variant，并通过类型化访问器暴露：

```cpp
std::string const&            str  = req.as_string();
boost::json::value const&     json = req.as_json();
httplib::form_data const&     fd   = req.as_form_data();
httplib::query_params const&  qp   = req.as_query_params();
```

`Content-Type` 头在运行时选择对应的读取器；`req.type()` 返回 `httplib::body_type`（`none`、`empty`、`string`、`json`、`query_params`、`form_data`）。

对于惰性处理器（body 尚未读取），需显式读取：

```cpp
auto body = co_await req.read_string();       // 也可 read_json / read_form_data / read_query_params
// 或流式读取原始 / 已解压字节：
std::size_t n = co_await req.read_some_raw(buffer, ec);
std::size_t m = co_await req.read_some_decompressed(buffer, ec);
co_await req.read_body();                      // 按 content-type 物化
```

## WebSocket

### 服务端

```cpp
router.set_ws_handler(
    "/ws",
    [](server::websocket_conn::weak_ptr hdl) -> net::awaitable<void> {
        if (auto conn = hdl.lock()) conn->send(websocket_message("Welcome!", false));
        co_return;
    },
    [](server::websocket_conn::weak_ptr hdl, websocket_message msg) -> net::awaitable<void> {
        if (auto conn = hdl.lock()) {
            conn->send(websocket_message(std::format("Echo: {}", msg.view()), msg.is_binary()));
        }
        co_return;
    },
    [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });
```

`websocket_conn` 还提供 `ping()` / `async_ping()`、`close()` / `async_close(reason)`、`abort()`，以及用于握手请求的 `http_request()`。WebSocket 处理器不接受中间件 aspect；如需鉴权，可在 open 回调里读取 `conn->http_request()` 自行校验。

### 客户端

```cpp
#include <httplib/client/ws_client.hpp>

client::ws_client ws(ex, "127.0.0.1", 8080, url::scheme::plain);

ws.run(
    "/ws",
    [](boost::system::error_code ec) -> net::awaitable<void> { co_return; },
    [](websocket_message msg) -> net::awaitable<void> { co_return; },
    []() -> net::awaitable<void> { co_return; });

// 自定义握手头
httplib::headers hdrs;
hdrs.set(httplib::field::authorization, "Bearer my-token");
ws.run("/ws", open, msg, close, hdrs);
```

也可单独调用各操作：`async_connect(target, headers, timeout, ec)`、
`async_send(msg, ec)`、`async_read(msg, ec)`、`async_ping(msg, ec)`、
`async_pong(msg, ec)`、`async_close(ec)`，以及返回
`std::future<error_code>` 的非阻塞版本 `send` / `ping` / `close`。
`set_verify_ssl()` / `set_ca_cert()` 在下次连接时生效。

`websocket_message` 拥有自身负载并携带帧类型
（`msg.is_text()` / `msg.is_binary()`，负载通过 `msg.data()` / `msg.view()` 获取）。

### 转发

```cpp
svr.set_ws_forward("/ws/proxy", "ws://upstream:8080/ws");
// 或使用动态 / 负载均衡上游（见"反向代理"），配合 ws_interceptor_factory
```

## 内置中间件

中间件可按路由应用（以变参形式传入），也可全局应用（`router::use()`）。每个中间件提供 `bool before(...)`，以及可选的 `after(...)`（同步或协程）。

### cors_middleware

```cpp
#include <httplib/server/middleware/cors.hpp>

router.set_http_handler<method::get>(
    "/api/data", handler,
    middleware::cors_middleware{}
        .allow_origin("https://example.com")
        .allow_methods({"GET", "POST"})
        .allow_headers({"Content-Type", "Authorization"})
        .allow_credentials(true)
        .max_age(3600));            // 秒
```

### Basic Auth

```cpp
#include <httplib/server/middleware/auth.hpp>

router.set_http_handler<method::get>(
    "/api/admin", handler,
    middleware::basic_auth_middleware(
        [](std::string_view user, std::string_view pass) {
            return user == "admin" && pass == "secret";
        }, "Admin Area"));
```

### Bearer Token Auth

```cpp
router.set_http_handler<method::get>(
    "/api/protected", handler,
    middleware::bearer_auth_middleware(
        [](std::string_view token) { return token == "my-secret-token"; }));
```

### JWT Auth

```cpp
#include <httplib/jwt.hpp>
#include <httplib/server/middleware/jwt_auth.hpp>

// 按路由
router.set_http_handler<method::get>(
    "/api/secure", handler,
    middleware::jwt_auth_middleware{httplib::jwt::hs256("secret")}
        .with_issuer("my-app")
        .with_audience("api.example.com"));

// 自定义 scheme / 头名
router.set_http_handler<method::get>(
    "/api/secure", handler,
    middleware::jwt_auth_middleware{httplib::jwt::hs256("secret")}
        .with_scheme("ApiKey")
        .with_header_name("X-API-Key"));

// 在处理器中读取已校验的 JWT
router.set_http_handler<method::get>(
    "/api/profile", [](server::request& req, server::response& resp) {
        auto jwt = middleware::fetch<middleware::jwt_auth_middleware>(req);
        auto sub = jwt.get_subject();
        bool has_exp = jwt.has_expires_at();
    },
    middleware::jwt_auth_middleware{httplib::jwt::hs256("secret")});
```

#### JWT Builder 与 Verifier

```cpp
// 创建并签名
auto token = jwt::create()
    .set_subject("alice")
    .set_issuer("my-app")
    .set_expires_in(std::chrono::hours(1))
    .sign(jwt::hs256("secret"));

// 解码（返回 result）
auto result = jwt::decode(token);
if (result.has_error()) return;

auto& decoded = result.value();
auto sub = decoded.get_subject();
auto iss = decoded.get_issuer();

// 带 claim 校验
auto verifier = jwt::verify(jwt::hs256("secret"))
    .with_issuer("my-app")
    .with_subject("alice")
    .with_claim("role", [](boost::json::value const& v) { return v == "admin"; });

boost::system::error_code ec;
verifier.verify(decoded, ec);
if (ec) { /* 校验失败 */ }
```

### 限流

```cpp
#include <httplib/server/middleware/rate_limit.hpp>

router.set_http_handler<method::get>(
    "/api/limited", handler,
    middleware::rate_limit_middleware(100, std::chrono::seconds(60)));
```

跟踪的客户端数有上限（默认 8192）并按空闲时间回收，内存不会无界增长：

```cpp
middleware::rate_limit_middleware limiter(100, std::chrono::seconds(60));
limiter.max_tracked_clients(8192)                        // 桶表容量硬上限
      .idle_expiration(std::chrono::minutes(5))         // 空闲桶保留时长
      .when_full(middleware::capacity_action::evict_oldest); // 桶满时的策略
```

`when_full` 有两种取值：`evict_oldest`（默认）淘汰最久未访问的桶腾位，每个请求
都被计数，限流不会因为桶满而失效；`reject` 直接返回 429，保护性更强，但攻击者
只要占满桶表就能把之后到达的新客户端挡在门外。

> 限流按 `request::get_client_ip()` 识别的客户端计数。**默认不信任任何代理**，
> 该函数一律返回 TCP 对端地址，忽略 `X-Forwarded-For`——否则任何客户端都能自己
> 声明 IP 从而绕过限流。只有当服务确实部署在反向代理之后时才需要配置可信来源：
>
> ```cpp
> svr.set_trusted_proxies({ "10.0.0.0/8", "::1/128" });
> ```
>
> 配好之后才会从 `X-Forwarded-For` **右往左**跳过同样可信的地址，取第一个非可信
> 地址作为真实客户端（取最左端会被客户端预置的伪造值骗过）。

### Session（基于 Cookie）

```cpp
#include <httplib/server/middleware/session.hpp>

middleware::session_middleware sm;                 // 默认 cookie 名："session_id"
sm.cookie_name("SID")
  .max_age(std::chrono::hours(24))
  .http_only(true)
  .secure(true)
  .same_site_lax();

router.set_http_handler<method::get>(
    "/api/profile", [](server::request& req, server::response& resp) {
        auto sess = middleware::fetch<middleware::session_middleware>(req);  // shared_ptr<session>
        sess->set("user", "alice");
        auto user = sess->get("user");
    }, sm);
```

可通过 `session_middleware(std::shared_ptr<session_store>)` 提供自定义存储后端。

### 数据库中间件

仅在启用 `HTTPLIB_ENABLED_DATABASE` 时可用（见"数据库"一节）。

```cpp
#include <httplib/server/middleware/db_middleware.hpp>

router.set_http_handler<method::get>(
    "/api/users", handler,
    middleware::db_middleware(pool, { .auto_transaction = true }));
```

### 全局中间件

一次性对所有路由应用中间件：

```cpp
router.use(
    middleware::cors_middleware{}.allow_origin("https://example.com"),
    middleware::basic_auth_middleware{[](auto...) { return true; }});

router.set_http_handler<method::get>("/api/a", handler_a);
router.set_http_handler<method::get>("/api/b", handler_b);
```

执行顺序：`global_before -> route_before -> handler -> route_after -> global_after`。

`set_post_routing_handler()` 是另一个独立钩子，对每个请求在路由处理器（及其 `after` aspect）之后运行。

### 自定义中间件

```cpp
struct logging_aspect {
    bool before(server::request& req, server::response&) {
        spdlog::info("[{}] {}", req.method_string(), req.path());
        return true;
    }
    bool after(server::request& req, server::response&) {
        spdlog::info("[{}] {} done", req.method_string(), req.path());
        return true;
    }
};

router.set_http_handler<method::get>("/api/logged", handler, logging_aspect{});
```

`before`/`after` 支持同步（`bool`）与协程（`net::awaitable<bool>`）两种返回类型。

### 中间件数据访问

在请求上存储数据的中间件（如 `jwt_auth_middleware`、`session_middleware`、`db_middleware`）都会暴露 `value_type`。使用通用模板 `middleware::fetch<>()` 读取：

```cpp
#include <httplib/server/middleware/data.hpp>

auto jwt  = middleware::fetch<middleware::jwt_auth_middleware>(req);   // jwt::decoded_jwt
auto sess = middleware::fetch<middleware::session_middleware>(req);   // shared_ptr<session>
```

通用辅助函数为 `middleware::fetch<MW>(req)`、`middleware::store<MW>(req, value)`、`middleware::has<MW>(req)`、`middleware::erase<MW>(req)`。它们封装了请求级原始存储：

```cpp
req.data().store(my_tag{42});          // 按值类型作 key
auto v = req.data().fetch<my_tag>();
bool exists = req.data().has<my_tag>();
req.data().erase<my_tag>();
```

## 反向代理

```cpp
// 简单的 URL 代理
svr.set_reverse_proxy("/api/*", "http://upstream:8080");

// 带每请求拦截器（改写头、观察 body 等）
svr.set_reverse_proxy("/api/*", "http://upstream:8080",
    [](server::request& req) -> std::shared_ptr<server::proxy_interceptor> {
        return nullptr;   // 或自定义拦截器
    });

// 动态选择上游
struct pick_upstream : server::upstream_provider {
    net::awaitable<std::string> url(server::request& req) override {
        co_return req.target().starts_with("/v1/") ? "http://a:8080" : "http://b:8080";
    }
};
svr.set_reverse_proxy("/api/*", std::make_shared<pick_upstream>());

// 负载均衡后端组
svr.set_reverse_proxy("/api/*",
    std::vector<server::upstream_backend>{ {"http://a:8080"}, {"http://b:8080", 2} },
    server::upstream_locator::least_connections);
```

`server::proxy_interceptor` 钩子：`on_upstream_request`、`on_upstream_request_body`、`on_upstream_response`、`on_upstream_response_body`。全局中间件同样作用于代理路由。

## 服务端配置

```cpp
svr.set_read_timeout(std::chrono::seconds(10));
svr.set_write_timeout(std::chrono::seconds(10));
svr.set_header_limit(16 * 1024);        // 请求头最大字节数
svr.set_body_limit(100 * 1024 * 1024);  // 请求体最大字节数
svr.set_logger(my_spdlog_logger);

svr.set_form_data_config(httplib::form_data::param{
    .save_dir = "/tmp/uploads",
    .max_file_size = 10 * 1024 * 1024,   // 每个 part；0 = 不限
    .max_fields = 128,
    .remove_uploaded_files = true });    // 请求结束后删除临时 part

svr.set_compress_content_types([](std::string_view ct) {
    return ct.starts_with("text/") || ct.starts_with("application/json");
});
```

上传的 part 以 `<16 位十六进制>_<客户端文件名>` 落盘，因此并发请求不会互相覆盖；真实路径从 `field::file_path`（即 `form_data::field::file_path`）读取。当 `remove_uploaded_files` 为 true 时，服务器在响应发送后删除每个 part；若想保留文件，请在处理器内把它移走。

本地端点（例如 `listen(port=0)` 之后）可通过 `svr.local_endpoint()` 获取。

## SSL/TLS

```cpp
#ifdef HTTPLIB_ENABLED_SSL
server::http_server svr(ex);

// 从内存（PEM 内容）
svr.set_ssl(cert_pem, key_pem, "password");

// 从文件
svr.set_ssl_file("server.crt", "server.key", "password");

// 客户端
client::http_client client(ex, "example.com", 443, url::scheme::tls);
client.set_verify_ssl(true);
client.set_ca_cert(ca_pem);   // PEM 编码的 CA 内容
```

## 静态文件服务

```cpp
#include <httplib/server/mount_point_entry.hpp>

server::mount_point_entry mp("/static", "/var/www");
mp.set_enabled_directory(true);
mp.set_directory_format(server::mount_point_entry::dir_format_type::html);   // 或 json
mp.set_default_document_name({ "index.html" });
router.set_static_mount_point(std::move(mp));

// 或挂载目录并附带中间件
router.set_static_mount_point("/secure-storage", "/data",
    middleware::basic_auth_middleware{...});
```

## 线程模型

本库基于 Boost.Asio 协程。几条最容易踩的约束：

- **协程不等于串行**：两个 `co_spawn` 到同一 strand 的协程仍会在 `co_await` 点交错。跨
  `co_await` 的不变量必须用 mutex / atomic 保护，strand 只保证「不同时执行」。
- **socket 读写靠 strand 串行化，路径上不加锁**：每次操作前重新取 `stream_` 快照，并发
  `close()` 只会让在途操作以错误码返回。
- **同一 client 同时最多一个请求在途**（single-flight）。此约束无运行时防护，违反是未定义行为。
- **路由仅配置期可写**：`httplib::server::router` 内部没有锁，运行期注册路由是 UB。
- **标量配置运行期可改**：一律 `std::atomic`；指针型配置用 `std::atomic<std::shared_ptr<T>>` 快照。

完整说明（各组件 strand 拓扑、`sessions_` 归属、停机时序、必须空闲时调用的接口清单）见
[THREAD_MODEL.md](THREAD_MODEL.md)。

## 客户端功能

```cpp
// 异步请求快捷方法
auto r1 = co_await client.async_get("/api/data");
auto r2 = co_await client.async_post("/api/json", boost::json::value{{"ok", true}});
auto r3 = co_await client.async_put("/api/data", "text", "text/plain");
auto r4 = co_await client.async_del("/api/data/1");
auto r5 = co_await client.async_options("/api/data");

// 以对象形式构造请求（方法 + target + 头 + body）
client::request req{httplib::method::post, "/upload"};
req.set(httplib::field::content_type, "application/json");
req.set_body(boost::json::value{{"k", "v"}});
auto r6 = co_await client.async_send_request(req);

// 惰性请求：自行流式写请求体
auto lr = client.create_lazy_request();
co_await lr->write_header(httplib::method::post, "/stream", headers,
                          client::lazy_request::mode::chunked);
co_await lr->write_body(net::buffer(chunk), /*more=*/true);
auto lresp = co_await lr->read_response_lazy();     // body 稍后再读

// 惰性响应：按需读取 body
auto resp = co_await client.async_send_request(req, client::http_client::body_mode::lazy);
if (resp) {
    auto body = co_await resp.value().read_string();   // result<std::string>
    // 或直接流式写入文件：
    // co_await resp.value().read_to_file("download.bin");
}

// 文件下载到磁盘（自动处理 chunked 与 content-encoding）
co_await client.async_download(httplib::method::get, "/files/large.bin", "local_copy.bin");

// 超时与重定向策略
client.set_timeout(std::chrono::seconds(5));
client.set_timeout_policy(client::http_client::timeout_policy::overall);   // 或 step / never
client.set_max_redirects(5);         // 0 = 不跟随重定向
client.set_download_rate_limit(1024 * 1024);
```

### Downloader

```cpp
#include <httplib/client/downloader.hpp>

client::downloader dl(ex, pool);   // pool 为 shared_ptr<client::http_client_pool>
dl.set_config({ .segments = 4, .resume = true });
dl.set_progress_callback([](client::downloader::progress_info const& p) { /* ... */ });
co_await dl.async_download("https://example.com/big.iso", "big.iso");
```

### Download Scheduler

```cpp
#include <httplib/client/download_scheduler.hpp>

client::download_scheduler sched(ex, pool, { .max_concurrent = 8 });
auto id = sched.add("https://example.com/a.bin", "a.bin");
auto st = co_await sched.async_wait_one(id);
co_await sched.async_run();   // 驱动所有任务直至完成
```

### Disk Cache

```cpp
#include <httplib/client/disk_cache.hpp>

auto cache = std::make_shared<client::disk_cache>("/tmp/httplib-cache");
cache->set_max_size(512ull * 1024 * 1024);
cache->set_max_age(std::chrono::hours(24));
dl.set_cache(cache);
```

### Proxy Client（原始隧道）

```cpp
#include <httplib/client/proxy_client.hpp>

client::proxy_client pc(ex, "proxy.internal", 3128);
boost::system::error_code ec;
co_await pc.async_connect("example.com:443", {}, ec);
```

## 数据库

使用 `-DHTTPLIB_ENABLED_DATABASE=ON` 启用。后端：`"sqlite"`、`"mysql"`、`"odbc"`。

```cpp
#include <httplib/db/session.hpp>
#include <httplib/db/config.hpp>
#include <httplib/db/binder.hpp>
#include <httplib/db/extractor.hpp>
#include <httplib/db/connection_pool.hpp>

namespace db = httplib::db;

// 用后端名 + 连接串建立连接
auto sess = co_await db::session::connect(ex, "sqlite", "db=:memory:");
// MySQL："host=127.0.0.1 port=3306 user=root password=... db=main"
// 配置结构：db::mysql_config / db::sqlite_config / db::odbc_config，均含 to_connection_string()

co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, n INTEGER)");

// 命名参数绑定；NULL 用 nullptr / std::nullopt
co_await sess.query("INSERT INTO t VALUES (:id, :name, :n)",
                    db::bind("id", 1),
                    db::bind("name", std::string("alice")),
                    db::bind("n", 42));

// 读取行
auto r = co_await sess.query("SELECT id, name, n FROM t ORDER BY id");
for (std::size_t i = 0; i < r.row_count(); ++i) {
    auto id   = r[i].as_int64("id");     // std::optional
    auto name = r[i].as_string("name");
}

// 直接提取到变量（vector = 全部行，optional = 第一行，标量 = 第一行）
std::vector<std::string> names;
co_await sess.query("SELECT name FROM t ORDER BY id", db::into(names));

// 预编译语句（从 session 创建，复用 session 的 LRU 缓存）
auto stmt = sess.stmt("INSERT INTO t VALUES (:id, :name, :n)");
co_await stmt.bind("id", 2).bind("name", "bob").bind("n", 7).execute();

// 事务
co_await sess.with_transaction([](db::session& s) -> net::awaitable<void> {
    co_await s.query("INSERT INTO t VALUES (3, 'carol', 9)");
});

// 连接池（借出 session；句柄析构时归还）
db::pool_params p;
p.min_connections = 2;
p.max_connections = 8;
auto pool = std::make_shared<db::connection_pool>(
    db::make_pool(ex, "sqlite", "db=app.db", p));
auto handle = co_await pool->async_acquire();
co_await handle->query("SELECT 1");
```

`sess.set_query_logger(cb)` 会为每条语句收到一个 `db::query_log_entry`。`db_middleware` 把 `db::connection_pool` 绑定到每个请求，并可选地用事务包裹处理器；`db_query_log_middleware` 聚合每个请求的查询日志：

```cpp
#include <httplib/server/middleware/db_query_log.hpp>

router.set_http_handler<method::get>("/api/users", handler,
    middleware::db_middleware(pool, { .auto_transaction = true }),
    middleware::db_query_log_middleware(middleware::query_log_options{
        .on_request_complete = [](server::request const& req, auto const& entries) { /* ... */ },
        .slow_query_threshold = std::chrono::milliseconds(200),
        .on_slow_query = [](db::query_log_entry const& e) { /* ... */ } }));
```

错误以 `db::db_exception` 抛出，其中携带 `boost::system::error_code`。

## URL 工具

```cpp
#include <httplib/url/url.hpp>

auto parsed = url::parse_url("https://example.com:8443/api?x=1");
if (parsed) {
    parsed->host;             // "example.com"
    parsed->effective_port(); // 8443
    parsed->is_ssl();         // true
    parsed->target();         // "/api?x=1"
    parsed->to_url();
}

auto target = url::resolve("/a/b", "../c");           // "/c"
auto host_hdr = url::make_host_value("example.com", 443, url::scheme::tls);
auto url_str  = url::make_url_value("example.com", 443, url::scheme::tls, "/api");
auto encoded  = url::url_encode("a b/c");
auto decoded  = url::url_decode("a%20b%2Fc");
```

## 示例

当 `HTTPLIB_ENABLED_EXAMPLES=ON`（根项目默认开启）时，所有示例都会被构建：

- `examples/demo` - 服务端 + 客户端 + WebSocket 演示，覆盖路由、中间件、流式、SSE/NDJSON 与连接池
- `examples/download_demo` - downloader / download scheduler 演示
- `examples/stress_test` - 类 wrk 压测工具（`--url`、`-c`、`-d`、`-X`、`--body`）

## 许可

本项目采用 Boost Software License, Version 1.0 发布，详见 `LICENSE`。
