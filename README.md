# httplib

A small, embeddable HTTP/1.1 & WebSocket server and client library for C++23, built on top of [Boost.Beast](https://boost.org).

## Features

- **Full HTTP/1.1 support** — GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS, CONNECT, TRACE
- **Coroutine-first** — all I/O operations are `boost::asio::awaitable` based
- **Synchronous client** — blocking HTTP client methods also available
- **WebSocket** — server and client with text/binary messaging, ping/pong
- **Flexible routing** — fixed paths, `:named` parameters, `{param:regex}` constraints, `*` wildcard
- **Multiple body types** — string, JSON (Boost.JSON), multipart form-data, URL-encoded forms, file serving, empty
- **Static file serving** — mount directories with Range/Content-Range support, directory listing (HTML/JSON)
- **Chunked streaming** — server streaming responses, client chunk handler
- **Redirects** — `resp.set_redirect(url)`
- **Compression** — Brotli content-encoding (optional)
- **SSL/TLS** — HTTPS and WSS support via OpenSSL (optional)
- **Built-in middleware** — CORS, Basic Auth, Bearer Auth, Rate Limiting, Session
- **Custom aspects** — per-route middleware with `before`/`after` hooks
- **Client connection pool** — RAII handles with automatic reuse
- **Logging** — integrated spdlog

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
    net::io_context ioc;
    server::http_server svr(ioc);

    svr.router().set_http_handler<http::verb::get>(
        "/api/hello",
        [](server::request&, server::response& resp) {
            resp.set_string_content("Hello, World!", "text/plain");
        });

    svr.listen("127.0.0.1", 8080);
    svr.async_run();
    ioc.run();
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

### Client Connection Pool

```cpp
#include <httplib/client/client_pool.hpp>

client::http_client_pool pool(ex, 4);  // max 4 connections
{
    auto handle = pool.acquire("127.0.0.1", 8080);
    if (handle) {
        auto resp = handle->get("/api/hello");
        // handle released back to pool on scope exit
    }
}
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
        auto path = req.path_param("*");  // remaining path
    });

// Multiple verbs on one route
router.set_http_handler<http::verb::get, http::verb::post>(
    "/api/multi", handler);

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

// File serving (with Range support)
resp.set_file_content("/path/to/file.pdf", req.base());

// Multipart form data
std::vector<html::form_data::field> fields = {
    {"name", "", "text/plain", "value"}
};
resp.set_form_data_content(std::move(fields));

// Chunked streaming
resp.set_stream_content(
    [idx = std::make_shared<int>(0)](
        beast::flat_buffer& buf,
        boost::system::error_code&) -> net::awaitable<bool> {
        if (*idx >= 5) co_return false;
        auto chunk = std::format("chunk #{}\n", (*idx)++);
        buf.commit(net::buffer_copy(buf.prepare(chunk.size()), net::buffer(chunk)));
        co_return true;
    },
    "text/plain");
```

## Body Types

The request/response body is a type-safe variant (`any_body::value_type`). Access it with `.as<T>()`:

```cpp
// String
auto& str = req.body().as<body::string_body>();

// JSON
auto& json = req.body().as<body::json_body>();

// Form data (multipart)
auto& fd = req.body().as<body::form_data_body>();

// URL-encoded form
auto& params = req.body().as<body::query_params_body>();
```

## WebSocket

### Server

```cpp
router.set_ws_handler(
    "/ws",
    // on_open
    [](server::websocket_conn::weak_ptr hdl) -> net::awaitable<void> {
        if (auto conn = hdl.lock())
            conn->send("Welcome!");
        co_return;
    },
    // on_message
    [](server::websocket_conn::weak_ptr hdl,
       std::string_view msg, bool binary) -> net::awaitable<void> {
        if (auto conn = hdl.lock())
            conn->send(std::format("Echo: {}", msg), binary);
        co_return;
    },
    // on_close
    [](server::websocket_conn::weak_ptr) -> net::awaitable<void> {
        co_return;
    });
```

### Client

```cpp
client::ws_client ws(ex, "127.0.0.1", 8080);
ws.set_handler(open_handler, message_handler, close_handler);
ws.async_run("/ws");
```

## Static File Serving

```cpp
server::mount_point_entry mp("/static", "/var/www");
mp.set_enabled_dir(true);                              // enable directory listing
mp.set_dir_format(mount_point_entry::dir_format_type::html);
router.set_static_mount_point(std::move(mp));
```

## Built-in Middleware

Middleware are applied per-route as trailing variadic arguments:

### CORS

```cpp
router.set_http_handler<http::verb::get>(
    "/api/data", handler,
    middleware::cors_middleware{}
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
    middleware::basic_auth_middleware(
        [](std::string_view user, std::string_view pass) {
            return user == "admin" && pass == "secret";
        },
        "Admin Area"));           // realm
```

### Bearer Token Auth

```cpp
router.set_http_handler<http::verb::get>(
    "/api/protected", handler,
    middleware::bearer_auth_middleware(
        [](std::string_view token) {
            return token == "my-secret-token";
        }));
```

### Rate Limiting

```cpp
auto limiter = std::make_shared<middleware::rate_limit_middleware>(
    100, std::chrono::seconds(60));   // 100 req / 60s per IP

router.set_http_handler<http::verb::get>(
    "/api/limited", handler, *limiter);
```

### Session

```cpp
#include <httplib/server/middleware/session.hpp>

auto sess = std::make_shared<middleware::session_middleware>();

sess->cookie_name("SID").max_age(std::chrono::hours(24)).http_only(true);

router.set_http_handler<http::verb::get>(
    "/api/session", handler, *sess);

// In handler:
router.set_http_handler<http::verb::get>(
    "/api/profile", [](server::request& req, server::response& resp) {
        auto session = req.session();
        session->set("user", "alice");
        auto user = session->get("user");
    }, *sess);
```

## Custom Aspects

Write your own middleware with `before`/`after` hooks:

```cpp
struct LoggingAspect {
    bool before(server::request& req, server::response&) {
        spdlog::info("[{}] {}", req.method_string(), req.path());
        return true;  // false to short-circuit the handler
    }
    bool after(server::request& req, server::response&) {
        spdlog::info("[{}] {} -> done", req.method_string(), req.path());
        return true;
    }
};

router.set_http_handler<http::verb::get>(
    "/api/logged", handler,
    LoggingAspect{});
```

Aspects can also be anonymous lambdas that only need `before()`:

```cpp
router.set_http_handler<http::verb::get>(
    "/api/data", handler,
    [](server::request& req, server::response&) {
        req.set_custom_data(std::make_any<std::string>("precomputed"));
        return true;
    });
```

## SSL/TLS

```cpp
server::http_server svr(ioc);

// From memory
svr.set_ssl(cert_pem, key_pem, "password");

// From files
svr.set_ssl_file("server.crt", "server.key", "password");
```

SSL clients simply pass `ssl=true`:

```cpp
client::http_client client(ex, "example.com", 443, true);
```

## License

This project is distributed under the Boost Software License, Version 1.0.
