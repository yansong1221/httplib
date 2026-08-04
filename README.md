# httplib

A small, embeddable HTTP/1.1 & WebSocket server and client library for C++23, built on [Boost.Beast](https://boost.org).

## Features

- **Full HTTP/1.1 support** — GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS, CONNECT, TRACE
- **Coroutine-first** — all I/O operations are `boost::asio::awaitable` based
- **Synchronous client** — blocking HTTP client methods also available
- **WebSocket** — server and client with text/binary messaging, ping/pong, middleware support
- **Flexible routing** — fixed paths, `:named` parameters, `{param:regex}` constraints, `*` wildcard
- **Multiple body types** — string, JSON (Boost.JSON), multipart form-data, URL-encoded forms, file serving, empty
- **Static file serving** — mount directories with Range/Content-Range support, directory listing (HTML/JSON)
- **Chunked streaming** — `chunk_writer` / `chunk_reader` for server and client
- **SSE (Server-Sent Events)** — `create_sse_writer()` / `sse_reader` streaming
- **NDJSON** — `create_ndjson_writer()` / `ndjson_reader` for newline-delimited JSON
- **Redirects** — `resp.set_redirect(url)`
- **Compression** — Brotli content-encoding (optional, `-DHTTPLIB_ENABLED_COMPRESS=ON`)
- **SSL/TLS** — HTTPS and WSS via OpenSSL (optional, `-DHTTPLIB_ENABLED_SSL=ON`)
- **JWT** — HS256/HS384/HS512 signing, verification, builder API, `boost::system::result` error handling
- **Built-in middleware** — CORS, Basic Auth, Bearer Auth, JWT Auth, Rate Limiting, Session (cookie-based)
- **Global middleware** — `router::use()` applies middleware to all routes
- **Custom middleware** — per-route `before`/`after` hooks, supports both sync and coroutine returns
- **Reverse proxy** — with Cookie/Referer rewriting, `X-Forwarded-*` headers
- **Client connection pool** — RAII handles, connection reuse, async acquire with backpressure
- **Logging** — integrated spdlog, configurable per-request detail level

## Platform Support

| Platform | Compiler |
|----------|----------|
| Windows  | MSVC, MinGW |
| Linux    | GCC, Clang |
| macOS    | GCC, Clang |
| FreeBSD  | Clang |

## Dependencies

| Library | Required | Notes |
|---------|----------|-------|
| Boost (beast, json) | Yes | HTTP/WebSocket & JSON |
| spdlog | Yes | Logging |
| fmt | Yes | String formatting |
| OpenSSL | Optional | SSL/TLS support (`HTTPLIB_ENABLED_SSL`) |
| Boost.Iostreams + Brotli | Optional | Compression (`HTTPLIB_ENABLED_COMPRESS`) |
| Catch2 | Tests only | Unit testing |

## Quick Start

```bash
mkdir build && cd build
cmake .. -DHTTPLIB_ENABLED_TESTS=ON
cmake --build .
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `HTTPLIB_ENABLED_SSL` | OFF | Enable HTTPS/WSS |
| `HTTPLIB_ENABLED_COMPRESS` | OFF | Enable Brotli compression |
| `HTTPLIB_ENABLED_EXAMPLES` | ON (root project) | Build examples |
| `HTTPLIB_ENABLED_TESTS` | ON (root project) | Build test suite |
| `HTTPLIB_SHARED_LIBRARY` | OFF | Build as shared library |

## Usage

### Server

```cpp
#include <httplib/server/server.hpp>
#include <httplib/server/response.hpp>

using namespace httplib;

int main() {
    net::thread_pool pool(4);
    server::http_server svr(pool.get_executor());

    svr.router().set_http_handler<http::verb::get>(
        "/api/hello",
        [](server::request&, server::response& resp) {
            resp.set_string_content("Hello, World!", "text/plain");
        });

    svr.listen("127.0.0.1", 8080);
    svr.run();
    pool.join();
}
```

### Client

```cpp
#include <httplib/client/client.hpp>

using namespace httplib;

int main() {
    net::io_context ioc;
    client::http_client client(ioc.get_executor(), "127.0.0.1", 8080);
    client.set_timeout(std::chrono::seconds(5));

    auto resp = client.get("/api/hello");
    if (resp) {
        auto body = resp->body().as<body::string_body>();
        std::cout << body << std::endl;
    }
}
```

### URL-Based Client

```cpp
client::http_client client(ex, "http://127.0.0.1:8080");
// or
client::http_client client(ex, "https://example.com");
```

### Client Connection Pool

```cpp
#include <httplib/client/client_pool.hpp>

client::http_client_pool pool(ex, 4);  // max 4 active connections

// Synchronous — returns std::future<client_handle>
{
    auto handle = pool.acquire("127.0.0.1", 8080).get();
    if (handle) {
        auto resp = handle->get("/api/hello");
        // handle released back to pool on scope exit
    }
}

// Asynchronous — waits when pool is at capacity
net::co_spawn(ex, []() -> net::awaitable<void> {
    auto handle = co_await pool.async_acquire("127.0.0.1", 8080);
    if (handle) {
        auto resp = co_await handle->async_get("/api/hello");
    }
    co_return;
}, net::detached);
```

## Routing

```cpp
auto& router = svr.router();

// Fixed path
router.set_http_handler<http::verb::get>("/api/hello", handler);

// Named parameter
router.set_http_handler<http::verb::get>(
    "/api/user/:id", [](server::request& req, server::response& resp) {
        auto id = req.path_param("id");
    });

// Regex constraint
router.set_http_handler<http::verb::get>(
    "/api/item/{id:^\\d+$}", handler);

// Wildcard
router.set_http_handler<http::verb::get>(
    "/api/files/*", [](server::request& req, server::response& resp) {
        auto path = req.path_param("*");
    });

// Member function handlers
router.set_http_handler<http::verb::get>(
    "/api/member", &MyClass::handler, my_instance);

// Custom 404
router.set_http_not_found_handler([](server::request& req, server::response& resp) {
    resp.set_string_content("Not found", "text/plain", http::status::not_found);
});
```

## Response Types

```cpp
// Plain text
resp.set_string_content("Hello", "text/plain");

// JSON
resp.set_json_content({{"ok", true}, {"data", 42}});

// Empty (204 No Content)
resp.set_empty_content(http::status::no_content);

// Error (500 with HTML body)
resp.set_error_content(http::status::internal_server_error);

// Redirect
resp.set_redirect("/new-location", http::status::moved_permanently);

// File serving (with Range/ETag/If-Modified-Since support)
resp.set_file_content("/path/to/file.pdf", req.base());

// Multipart form data
resp.set_form_data_content({
    {"name", "", "text/plain", "value"}
});
```

### Chunked Streaming

```cpp
resp.set_http_handler<http::verb::get>(
    "/stream",
    [](server::request&, server::response& resp) -> net::awaitable<void> {
        auto* writer = resp.get_chunk_writer();
        for (int i = 0; i < 5; ++i)
            co_await writer->write_chunk(std::format("chunk #{}\n", i));
        co_await writer->close();
    });
```

### SSE (Server-Sent Events)

```cpp
resp.set_http_handler<http::verb::get>(
    "/api/sse", [](server::request&, server::response& resp) -> net::awaitable<void> {
        auto sse = resp.create_sse_writer();
        auto counter = std::make_shared<int>(0);
        while (true) {
            ++(*counter);
            co_await sse->send_event(
                std::format("event #{}", *counter), "tick", std::to_string(*counter));
        }
    });
```

`create_sse_writer()` returns `unique_ptr<sse_writer>`. `sse_writer` methods: `send_event(data)`, `send_event(data, event)`, `send_event(data, event, id)`, `send_retry(ms)`, `send_comment(msg)`.

### NDJSON (Newline Delimited JSON)

```cpp
resp.set_http_handler<http::verb::get>(
    "/api/ndjson", [](server::request&, server::response& resp) -> net::awaitable<void> {
        auto w = resp.create_ndjson_writer();
        co_await w->write({{"seq", 1}, {"msg", "hello"}});
        co_await w->write({{"seq", 2}, {"msg", "world"}});
        co_await w->close();
    });
```

## Body Types

The request/response body is a type-safe variant (`any_body::value_type`). Access with `.as<T>()`:

```cpp
auto& str  = req.body().as<body::string_body>();
auto& json = req.body().as<body::json_body>();
auto& fd   = req.body().as<body::form_data_body>();
auto& qp   = req.body().as<body::query_params_body>();
```

Auto-detection during parsing: `content-type` header selects the correct body reader at runtime.

## WebSocket

### Server

```cpp
router.set_ws_handler(
    "/ws",
    [](server::websocket_conn::weak_ptr hdl) -> net::awaitable<void> {
        if (auto conn = hdl.lock()) conn->send("Welcome!");
        co_return;
    },
    [](server::websocket_conn::weak_ptr hdl, std::string_view msg, bool binary) -> net::awaitable<void> {
        if (auto conn = hdl.lock()) conn->send(std::format("Echo: {}", msg), binary);
        co_return;
    },
    [](server::websocket_conn::weak_ptr) -> net::awaitable<void> { co_return; });
```

WebSocket handlers support middleware aspects:

```cpp
router.set_ws_handler("/ws", open, msg, close,
    middleware::basic_auth{[](std::string_view u, std::string_view p) {
        return u == "admin" && p == "secret";
    }});
```

### Client

```cpp
client::ws_client ws(ex, "127.0.0.1", 8080);
ws.set_handler(
    [](boost::system::error_code ec) -> net::awaitable<void> { co_return; },
    [](std::string_view msg, bool binary) -> net::awaitable<void> { co_return; },
    []() -> net::awaitable<void> { co_return; });

// With custom headers
auto hdrs = http::fields();
hdrs.set(http::field::authorization, "Bearer my-token");
ws.run("/ws", hdrs);
```

## Built-in Middleware

Middleware can be applied per-route (variadic arguments) or globally (`router::use()`).

### CORS

```cpp
router.set_http_handler<http::verb::get>(
    "/api/data", handler,
    middleware::cors{}
        .allow_origin("https://example.com")
        .allow_methods("GET, POST")
        .allow_headers("Content-Type, Authorization")
        .allow_credentials(true)
        .max_age(std::chrono::hours(1)));
```

### Basic Auth

```cpp
router.set_http_handler<http::verb::get>(
    "/api/admin", handler,
    middleware::basic_auth(
        [](std::string_view user, std::string_view pass) {
            return user == "admin" && pass == "secret";
        }, "Admin Area"));
```

### Bearer Token Auth

```cpp
router.set_http_handler<http::verb::get>(
    "/api/protected", handler,
    middleware::bearer_auth(
        [](std::string_view token) { return token == "my-secret-token"; }));
```

### JWT Auth

```cpp
#include <httplib/server/middleware/jwt_auth.hpp>
#include <httplib/util/jwt.hpp>

// Per-route
router.set_http_handler<http::verb::get>(
    "/api/secure", handler,
    middleware::jwt_auth{httplib::jwt::hs256("secret")}
        .with_issuer("my-app")
        .with_audience("api.example.com"));

// Custom scheme / header name
router.set_http_handler<http::verb::get>(
    "/api/secure", handler,
    middleware::jwt_auth{httplib::jwt::hs256("secret")}
        .with_header_name("X-API-Key"));       // read from custom header

// Access verified JWT in handler
router.set_http_handler<http::verb::get>(
    "/api/profile", [](server::request& req, server::response& resp) {
        auto& jwt = middleware::get_data<middleware::jwt_auth>(req);
        auto sub = jwt.get_subject();
        bool has_exp = jwt.has_expires_at();
    },
    middleware::jwt_auth{httplib::jwt::hs256("secret")});
```

#### JWT Builder & Verifier

```cpp
// Create & sign
auto token = jwt::create()
    .set_subject("alice")
    .set_issuer("my-app")
    .set_expires_in(std::chrono::hours(1))
    .sign(jwt::hs256("secret"));

// Decode (returns result)
auto result = jwt::decode(token);
if (result.has_error()) return;

auto& decoded = result.value();
auto sub = decoded.get_subject();
auto iss = decoded.get_issuer();

// Verify with claims
auto verifier = jwt::verify(jwt::hs256("secret"))
    .with_issuer("my-app")
    .with_subject("alice")
    .with_claim("role", [](auto& v) { return v == "admin"; });

boost::system::error_code ec;
verifier.verify(decoded, ec);
if (ec) { /* verification failed */ }
```

### Rate Limiting

```cpp
router.set_http_handler<http::verb::get>(
    "/api/limited", handler,
    middleware::rate_limit(100, std::chrono::seconds(60)));
```

### Session

```cpp
#include <httplib/server/middleware/session.hpp>

auto sm = middleware::session_middleware()
    .cookie_name("SID")
    .max_age(std::chrono::hours(24))
    .http_only(true);

router.set_http_handler<http::verb::get>(
    "/api/profile", [](server::request& req, server::response& resp) {
        auto sess = middleware::get_data<middleware::session_middleware>(req);
        sess->set("user", "alice");
        auto user = sess->get("user");
    }, sm);
```

### Global Middleware

Apply middleware to all routes at once:

```cpp
router.use(
    middleware::cors{}
        .allow_origin("https://example.com"),
    middleware::basic_auth{[](auto...) { return true; }});

// All subsequent route registrations inherit these.
router.set_http_handler<http::verb::get>("/api/a", handler_a);
router.set_http_handler<http::verb::get>("/api/b", handler_b);
```

Execution order: `global_before → route_before → handler → route_after → global_after`.

### Custom Middleware

```cpp
struct LoggingAspect {
    bool before(server::request& req, server::response&) {
        spdlog::info("[{}] {}", req.method_string(), req.path());
        return true;
    }
    bool after(server::request& req, server::response&) {
        spdlog::info("[{}] {} done", req.method_string(), req.path());
        return true;
    }
};

router.set_http_handler<http::verb::get>("/api/logged", handler, LoggingAspect{});
```

Both sync (`bool`) and coroutine (`net::awaitable<bool>`) return types are supported for `before`/`after`.

### Middleware Data Access

Middleware that stores data in the request (like `jwt_auth` and `session_middleware`) exposes a `key` and `value_type`. Use the generic `get_data<>()` template:

```cpp
auto& jwt  = middleware::get_data<middleware::jwt_auth>(req);
auto  sess = middleware::get_data<middleware::session_middleware>(req);
```

The same `data` constant is used internally by `set_custom_data` → `request::custom_data`. You can also use the raw `custom_data` API:

```cpp
req.set_custom_data("my_key", std::make_any<int>(42));
req.erase_custom_data("my_key");
bool exists = req.has_custom_data("my_key");
```

## Reverse Proxy

```cpp
// Simple URL proxy
svr.set_reverse_proxy("/api/*", "http://upstream:8080");

// With custom header transformation
svr.set_reverse_proxy("/api/*", "http://upstream:8080",
    [](server::request& req, http::fields& headers) {
        headers.set("X-Custom", "value");
    });

// Dynamic upstream selection
svr.set_reverse_proxy("/api/*",
    [](server::request& req) -> std::string {
        return "http://backend-" + std::string(req.path_param("id")) + ":8080";
    },
    headers_callback);
```

### Proxy Config

```cpp
svr.set_proxy_pool_size(32);           // max idle connections to upstream
svr.set_proxy_buffer_size(256 * 1024); // relay buffer (default 512KB)
```

Global middleware applies to proxy routes automatically. Use `set_http_handler` on the same path for route-specific proxy middleware.

## Server Configuration

```cpp
svr.set_read_timeout(std::chrono::seconds(10));
svr.set_write_timeout(std::chrono::seconds(10));
svr.set_acceptor_count(64);     // concurrent listen sockets (default 32)
svr.set_upload_dir("/tmp/uploads");
svr.set_upload_file_limit(10 * 1024 * 1024);  // 10MB
svr.set_compress_content_types([](std::string_view ct) {
    return ct.starts_with("text/") || ct.starts_with("application/json");
});
```

## SSL/TLS

```cpp
server::http_server svr(ex);

// From memory
svr.set_ssl(cert_pem, key_pem, "password");

// From files
svr.set_ssl_file("server.crt", "server.key", "password");

// Client
client::http_client client(ex, "example.com", 443, true);
// Verify server certificate
client.set_verify_ssl(true);
client.set_ca_cert("/path/to/ca.pem");
```

## Static File Serving

```cpp
server::mount_point_entry mp("/static", "/var/www");
mp.set_enabled_directory(true);
mp.set_directory_format(server::mount_point_entry::dir_format_type::html);
router.set_static_mount_point(std::move(mp));

// With middleware
router.set_static_mount_point("/secure-storage", "/data",
    middleware::basic_auth{...});
```

## Client Features

```cpp
// Async request
auto resp = co_await client.async_get("/api/data");

// File upload via multipart
consumer.set_http_handler<http::verb::post>(
    "/upload", [](server::request& req, server::response& resp) {
        auto& fd = req.body().as<body::form_data_body>();
        resp.set_string_content("OK", "text/plain");
    });

// File download to disk
client.async_download("/files/large.bin", "local_copy.bin");

// Timeout policies
client.set_timeout(std::chrono::seconds(5));           // per-step
client.set_overall_timeout(std::chrono::seconds(30));  // total
client.set_timeout_policy(client::timeout_policy::never);

// Follow redirects
client.set_max_redirects(5);
```

## License

This project is distributed under the Boost Software License, Version 1.0.
