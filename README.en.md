# httplib

English | [简体中文](README.md)

A small, embeddable HTTP/1.1 & WebSocket server and client library for C++23, built on [Boost.Beast](https://boost.org) and [Boost.Asio](https://boost.org).

Version: see `httplib::version()` (`<httplib/version.hpp>`).

## Features

- **Full HTTP/1.1 support** - GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS, CONNECT, TRACE
- **Coroutine-first** - all I/O operations are `boost::asio::awaitable` based; a request handler may be a plain callable or a coroutine
- **Async client** - `async_*` methods plus a connection pool, lazy streaming requests and ranged downloads
- **WebSocket** - server and client with text/binary messaging, ping/pong, disconnect callbacks
- **WebSocket forwarding** - reverse-proxy live WebSocket sessions with interceptors
- **Flexible routing** - fixed paths, `:named` parameters, `{param:regex}` constraints, `*` wildcard, member-function handlers, multi-verb registration
- **Multiple body types** - string, JSON (Boost.JSON), multipart form-data, URL-encoded forms, file bodies, empty
- **Streaming bodies** - `set_lazy_http_handler` + `request::read_some_raw()` / `read_string()` / `read_json()` for request streaming; `stream_writer` for responses
- **SSE (Server-Sent Events)** - `create_sse_writer()` (server) / `create_sse_reader()` (client)
- **NDJSON** - `create_ndjson_writer()` (server) / `create_ndjson_reader()` (client)
- **Redirects** - `resp.set_redirect(url, status)`, client-side redirect following
- **Compression** - Brotli content-encoding (optional, `-DHTTPLIB_ENABLED_COMPRESS=ON`)
- **SSL/TLS** - HTTPS and WSS via OpenSSL (optional, `-DHTTPLIB_ENABLED_SSL=ON`)
- **JWT** - HS256/HS384/HS512 signing, verification, builder API, `boost::system::result` error handling
- **Built-in middleware** - CORS, Basic Auth, Bearer Auth, JWT Auth, Rate Limiting, Session (cookie-based), DB access / query logging
- **Global middleware** - `router::use()` applies middleware to all routes; `set_post_routing_handler()` runs after every handler
- **Custom middleware** - per-route `before`/`after` aspects, sync (`bool`) or coroutine (`awaitable<bool>`) returns
- **Reverse proxy** - static or dynamic upstreams, weighted/least-connections load balancing, cookie/referer rewriting, `X-Forwarded-*` headers, request/response interceptors
- **Client download stack** - `downloader` (resume, progress), `download_scheduler` (bounded concurrency), `disk_cache` (size/TTL eviction)
- **Database** - backend-agnostic `session` with parameter binding, prepared statements, transactions and pooling; SQLite / MySQL / ODBC backends (optional, `-DHTTPLIB_ENABLED_DATABASE=ON`)
- **URL utilities** - parsing, resolution, percent encoding, Host/URL assembly
- **Logging** - integrated spdlog, configurable per-component loggers

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
| Boost (asio, beast, json, url) | Yes | HTTP/WebSocket, JSON, URL parsing |
| spdlog | Yes | Logging |
| fmt | Yes | String formatting |
| OpenSSL | Optional | SSL/TLS (`HTTPLIB_ENABLED_SSL`) |
| Boost.Iostreams + Brotli | Optional | Compression (`HTTPLIB_ENABLED_COMPRESS`) |
| Boost.MySQL (1.85+), Boost.Charconv | Optional | MySQL backend (`HTTPLIB_ENABLED_DATABASE`) |
| SQLite3 | Optional | SQLite backend (`HTTPLIB_ENABLED_DATABASE`) |
| ODBC (unixODBC / `odbc32`) | Optional | ODBC backend (`HTTPLIB_ENABLED_DATABASE`) |
| Catch2, jwt-cpp | Tests only | Test suite |

## Quick Start

```bash
mkdir build && cd build
cmake .. -DHTTPLIB_ENABLED_TESTS=ON
cmake --build .
```

Build output goes to `bin/x64/[Debug|Release]/` (or `bin/x86/` on 32-bit).

### Tests

```bash
ctest --test-dir build -C Debug          # all tests
ctest --test-dir build -C Debug -L http  # filter by label
```

Labels: `core`, `http`, `client`, `proxy`, `jwt`, `db`.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `HTTPLIB_ENABLED_SSL` | OFF | Enable HTTPS/WSS (OpenSSL) |
| `HTTPLIB_ENABLED_COMPRESS` | OFF | Enable Brotli compression |
| `HTTPLIB_ENABLED_DATABASE` | OFF | Enable the database module (Boost.MySQL 1.85+, SQLite3, ODBC) |
| `HTTPLIB_ENABLED_EXAMPLES` | ON (root project) | Build examples |
| `HTTPLIB_ENABLED_TESTS` | ON (root project) | Build test suite |
| `HTTPLIB_SHARED_LIBRARY` | OFF | Build as a shared library |
| `HTTPLIB_ENABLED_UNITY_BUILD` | OFF | Enable unity/jumbo builds (tests stay TU-isolated) |

## Usage

### Server

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
    svr.run();        // returns a future; also async_run(ec)
    pool.join();
}
```

### Client

The client is asynchronous; every request returns a `net::awaitable<boost::system::result<client::response>>`.

```cpp
#include <httplib/client/client.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <spdlog/spdlog.h>

using namespace httplib;

net::awaitable<void> run(net::any_io_executor ex) {
    client::http_client client(ex, "127.0.0.1", 8080);   // or url::scheme::tls
    client.set_timeout(std::chrono::seconds(5));

    auto resp = co_await client.async_get("/api/hello");
    if (resp) {
        spdlog::info("{} {}", resp->result_int(), resp->as_string());
    }
}
```

### URL-Based Client

```cpp
client::http_client client(ex, "http://127.0.0.1:8080");
client::http_client secure(ex, "https://example.com");   // TLS inferred from scheme
```

### Client Connection Pool

```cpp
#include <httplib/client/client_pool.hpp>

client::http_client_pool pool(ex, { .max_size = 4 });   // 4 connections per host

net::co_spawn(ex, [&]() -> net::awaitable<void> {
    auto handle = co_await pool.async_acquire("127.0.0.1", 8080, url::scheme::plain);
    if (handle) {
        auto resp = co_await handle->async_get("/api/hello");
    }
    co_return;   // handle returns the connection on destruction
}, net::detached);
```

`async_acquire` also accepts a full URL: `co_await pool.async_acquire("http://host:8080")`.

## Routing

```cpp
auto& router = svr.router();

// Fixed path
router.set_http_handler<method::get>("/api/hello", handler);

// Multiple verbs at once
router.set_http_handler<method::get, method::head>("/api/thing", handler);

// Named parameter (returns string_view by default; use path_param<int>("id") for other types)
router.set_http_handler<method::get>(
    "/api/user/:id", [](server::request& req, server::response& resp) {
        auto id = req.path_param("id");
    });

// Regex constraint
router.set_http_handler<method::get>("/api/item/{id:^\\d+$}", handler);

// Wildcard
router.set_http_handler<method::get>(
    "/api/files/*", [](server::request& req, server::response& resp) {
        auto path = req.path_param("*");
    });

// Member function handlers
router.set_http_handler<method::get>("/api/member", &MyClass::handler, my_instance);

// CONNECT is routed like any other verb
router.set_connect_handler("/tunnel", handler);

// Lazy handler: body is streamed, not materialized up front
router.set_lazy_http_handler<method::post>("/api/buffer", handler);

// Runs after every route handler (e.g. global response headers)
router.set_post_routing_handler([](server::request&, server::response& resp) {
    resp.set(httplib::field::server, "httplib");
});

// Custom 404
router.set_http_not_found_handler([](server::request& req, server::response& resp) {
    resp.set_string_content("Not found", "text/plain", httplib::status::not_found);
});
```

Note: the public API uses the library enums `httplib::method` and `httplib::status` (not `boost::beast::http`) and the `httplib::headers` / `httplib::field` types, so Boost types do not leak into user code.

## Response Types

```cpp
// Plain text
resp.set_string_content("Hello", "text/plain");

// JSON
resp.set_json_content({{"ok", true}, {"data", 42}});

// Empty (204 No Content)
resp.set_empty_content(httplib::status::no_content);

// Error (default HTML body)
resp.set_error_content(httplib::status::internal_server_error);

// Redirect (status defaults to moved_permanently)
resp.set_redirect("/new-location", httplib::status::moved_permanently);

// File serving (Range / ETag / If-Modified-Since aware)
resp.set_file_content("/path/to/file.pdf", req.base());

// Multipart form data
resp.set_form_data_content({
    form_data::field{ .name = "name", .content_type = "text/plain", .content = "value" }
});
```

### Chunked Streaming

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

### SSE (Server-Sent Events)

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

`create_sse_writer()` returns `unique_ptr<sse_writer>`. Methods: `begin()`, `send_event(data, event, id, more)`, `send_retry(ms, more)`, `send_comment(msg, more)`. Every method has an `error_code&` overload.

### NDJSON (Newline Delimited JSON)

```cpp
router.set_http_handler<method::get>(
    "/api/ndjson", [](server::request&, server::response& resp) -> net::awaitable<void> {
        auto w = resp.create_ndjson_writer();
        co_await w->begin();
        co_await w->write({{"seq", 1}, {"msg", "hello"}}, /*more=*/true);
        co_await w->write({{"seq", 2}, {"msg", "world"}}, /*more=*/false);
    });
```

## Body Types

The request body is materialized into a type-safe variant and exposed through typed accessors:

```cpp
std::string const&            str  = req.as_string();
boost::json::value const&     json = req.as_json();
httplib::form_data const&     fd   = req.as_form_data();
httplib::query_params const&  qp   = req.as_query_params();
```

The `Content-Type` header selects the reader at runtime; `req.type()` returns an `httplib::body_type` (`none`, `empty`, `string`, `json`, `query_params`, `form_data`).

For lazy handlers (body not read yet), read it explicitly:

```cpp
auto body = co_await req.read_string();       // also read_json / read_form_data / read_query_params
// or stream raw / decompressed bytes:
std::size_t n = co_await req.read_some_raw(buffer, ec);
std::size_t m = co_await req.read_some_decompressed(buffer, ec);
co_await req.read_body();                      // materialize according to content-type
```

## WebSocket

### Server

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

`websocket_conn` also exposes `ping()` / `async_ping()`, `close()` / `async_close(reason)`, `abort()`, and `http_request()` for the handshake request. Middleware aspects are not applied to WebSocket handlers; authenticate inside the open handler using `conn->http_request()` if needed.

### Client

```cpp
#include <httplib/client/ws_client.hpp>

client::ws_client ws(ex, "127.0.0.1", 8080, url::scheme::plain);

ws.run(
    "/ws",
    [](boost::system::error_code ec) -> net::awaitable<void> { co_return; },
    [](websocket_message msg) -> net::awaitable<void> { co_return; },
    []() -> net::awaitable<void> { co_return; });

// With custom handshake headers
httplib::headers hdrs;
hdrs.set(httplib::field::authorization, "Bearer my-token");
ws.run("/ws", open, msg, close, hdrs);
```

Individual operations are also available: `async_connect(target, headers, timeout, ec)`,
`async_send(msg, ec)`, `async_read(msg, ec)`, `async_ping(msg, ec)`,
`async_pong(msg, ec)`, `async_close(ec)`, plus non-blocking `send` / `ping` /
`close` returning `std::future<error_code>`. `set_verify_ssl()` / `set_ca_cert()`
are honored on the next connection.

A `websocket_message` owns its payload and carries the frame type
(`msg.is_text()` / `msg.is_binary()`, payload via `msg.data()` / `msg.view()`).

### Forwarding

```cpp
svr.set_ws_forward("/ws/proxy", "ws://upstream:8080/ws");
// or a dynamic/load-balanced upstream (see Reverse Proxy) with a ws_interceptor_factory
```

## Built-in Middleware

Middleware can be applied per-route (variadic arguments) or globally (`router::use()`). Each middleware provides `bool before(...)` and optionally `after(...)` (sync or coroutine).

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
        .max_age(3600));            // seconds
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

// Per-route
router.set_http_handler<method::get>(
    "/api/secure", handler,
    middleware::jwt_auth_middleware{httplib::jwt::hs256("secret")}
        .with_issuer("my-app")
        .with_audience("api.example.com"));

// Custom scheme / header name
router.set_http_handler<method::get>(
    "/api/secure", handler,
    middleware::jwt_auth_middleware{httplib::jwt::hs256("secret")}
        .with_scheme("ApiKey")
        .with_header_name("X-API-Key"));

// Access the verified JWT in the handler
router.set_http_handler<method::get>(
    "/api/profile", [](server::request& req, server::response& resp) {
        auto jwt = middleware::fetch<middleware::jwt_auth_middleware>(req);
        auto sub = jwt.get_subject();
        bool has_exp = jwt.has_expires_at();
    },
    middleware::jwt_auth_middleware{httplib::jwt::hs256("secret")});
```

#### JWT Builder & Verifier

```cpp
// Create & sign
auto token = jwt::create()
    .set_subject("alice")
    .set_issuer("my-app")
    .set_expires_in(std::chrono::hours(1))
    .sign(jwt::hs256("secret"));

// Decode (returns a result)
auto result = jwt::decode(token);
if (result.has_error()) return;

auto& decoded = result.value();
auto sub = decoded.get_subject();
auto iss = decoded.get_issuer();

// Verify with claims
auto verifier = jwt::verify(jwt::hs256("secret"))
    .with_issuer("my-app")
    .with_subject("alice")
    .with_claim("role", [](boost::json::value const& v) { return v == "admin"; });

boost::system::error_code ec;
verifier.verify(decoded, ec);
if (ec) { /* verification failed */ }
```

### Rate Limiting

```cpp
#include <httplib/server/middleware/rate_limit.hpp>

router.set_http_handler<method::get>(
    "/api/limited", handler,
    middleware::rate_limit_middleware(100, std::chrono::seconds(60)));
```

### Session (cookie-based)

```cpp
#include <httplib/server/middleware/session.hpp>

middleware::session_middleware sm;                 // default cookie name: "session_id"
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

A custom backing store can be supplied via `session_middleware(std::shared_ptr<session_store>)`.
`session_store` has three pure virtuals and a small contract:

- `load(id)` returns `std::optional<session>` — a **value snapshot** of the stored session, or
  `nullopt` when it is missing or expired. Returning by value is mandatory: handing out a reference
  or alias to internal state would let two concurrent requests carrying the same session_id mutate
  the same data.
- `save(id, writes)` applies the `session_writes` patch **atomically, key by key** to `id` (creating
  it when absent). The patch holds only the keys this request actually `set`/`remove`d, so two
  concurrent requests starting from the same baseline snapshot cannot overwrite each other — that is
  why commits are incremental rather than whole-session replacements. A deletion is expressed as
  `nullopt`, so `session::remove()` is never silently mistaken for "never existed".
- `destroy(id)` removes the session. It is not sticky: the store keeps no record of the deletion, so
  any later commit carrying a patch recreates the session.

The patch is recorded by `session` itself (`pending_writes()` / `take_pending_writes()`); the store
never sees or needs any of `session`'s internal state.

### Database Middleware

Only with `HTTPLIB_ENABLED_DATABASE` (see the Database section).

```cpp
#include <httplib/server/middleware/db_middleware.hpp>

router.set_http_handler<method::get>(
    "/api/users", handler,
    middleware::db_middleware(pool, { .auto_transaction = true }));
```

### Global Middleware

Apply middleware to every route at once:

```cpp
router.use(
    middleware::cors_middleware{}.allow_origin("https://example.com"),
    middleware::basic_auth_middleware{[](auto...) { return true; }});

router.set_http_handler<method::get>("/api/a", handler_a);
router.set_http_handler<method::get>("/api/b", handler_b);
```

Execution order: `global_before -> route_before -> handler -> route_after -> global_after`.

`set_post_routing_handler()` is a separate hook that runs after the route handler (and after its `after` aspects) for every request.

### Custom Middleware

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

Both sync (`bool`) and coroutine (`net::awaitable<bool>`) return types are supported for `before`/`after`.

### Middleware Data Access

Middleware that stores data on the request (like `jwt_auth_middleware`, `session_middleware`, `db_middleware`) exposes a `value_type`. Read it with the generic `middleware::fetch<>()` template:

```cpp
#include <httplib/server/middleware/data.hpp>

auto jwt  = middleware::fetch<middleware::jwt_auth_middleware>(req);   // jwt::decoded_jwt
auto sess = middleware::fetch<middleware::session_middleware>(req);   // shared_ptr<session>
```

The generic helpers are `middleware::fetch<MW>(req)`, `middleware::store<MW>(req, value)`, `middleware::has<MW>(req)` and `middleware::erase<MW>(req)`. They wrap the raw request-scoped storage:

```cpp
req.data().store(my_tag{42});                       // keyed by value type
auto v = req.data().fetch<my_tag>();           // std::optional<my_tag>
bool exists = req.data().has<my_tag>();            // predicate only, no copy
req.data().erase<my_tag>();
```

## Reverse Proxy

```cpp
// Simple URL proxy
svr.set_reverse_proxy("/api/*", "http://upstream:8080");

// With a per-request interceptor (rewrite headers, observe bodies, ...)
svr.set_reverse_proxy("/api/*", "http://upstream:8080",
    [](server::request& req) -> std::shared_ptr<server::proxy_interceptor> {
        return nullptr;   // or a custom interceptor
    });

// Dynamic upstream selection
struct pick_upstream : server::upstream_provider {
    net::awaitable<std::string> url(server::request& req) override {
        co_return req.target().starts_with("/v1/") ? "http://a:8080" : "http://b:8080";
    }
};
svr.set_reverse_proxy("/api/*", std::make_shared<pick_upstream>());

// Load-balanced backend group
svr.set_reverse_proxy("/api/*",
    std::vector<server::upstream_backend>{ {"http://a:8080"}, {"http://b:8080", 2} },
    server::upstream_locator::least_connections);
```

`server::proxy_interceptor` hooks: `on_upstream_request`, `on_upstream_request_body`, `on_upstream_response`, `on_upstream_response_body`. Global middleware applies to proxy routes as well.

## Server Configuration

```cpp
svr.set_read_timeout(std::chrono::seconds(10));
svr.set_write_timeout(std::chrono::seconds(10));
svr.set_header_limit(16 * 1024);        // max request header bytes
svr.set_body_limit(100 * 1024 * 1024);  // max request body bytes
svr.set_logger(my_spdlog_logger);

svr.set_form_data_config(httplib::form_data::param{
    .save_dir = "/tmp/uploads",
    .max_file_size = 10 * 1024 * 1024,   // per part; 0 = unlimited
    .max_fields = 128,
    .remove_uploaded_files = true });    // delete temp parts after the request

svr.set_compress_content_types([](std::string_view ct) {
    return ct.starts_with("text/") || ct.starts_with("application/json");
});
```

Uploaded parts are written as `<16 hex>_<client filename>` so concurrent requests never overwrite each other; read the real path from `field::file_path` (i.e. `form_data::field::file_path`). With `remove_uploaded_files` the server deletes each part once the response has been sent; move the file inside the handler to keep it.

The local endpoint (useful after `listen(port=0)`) is available via `svr.local_endpoint()`.

## SSL/TLS

```cpp
#ifdef HTTPLIB_ENABLED_SSL
server::http_server svr(ex);

// From memory (PEM contents)
svr.set_ssl(cert_pem, key_pem, "password");

// From files
svr.set_ssl_file("server.crt", "server.key", "password");

// Client
client::http_client client(ex, "example.com", 443, url::scheme::tls);
client.set_verify_ssl(true);
client.set_ca_cert(ca_pem);   // PEM-encoded CA contents
#endif
```

## Static File Serving

```cpp
#include <httplib/server/mount_point_entry.hpp>

server::mount_point_entry mp("/static", "/var/www");
mp.set_enabled_directory(true);
mp.set_directory_format(server::mount_point_entry::dir_format_type::html);   // or json
mp.set_default_document_name({ "index.html" });
router.set_static_mount_point(std::move(mp));

// Or mount a directory with middleware
router.set_static_mount_point("/secure-storage", "/data",
    middleware::basic_auth_middleware{...});
```

## Threading Model

This library is built on Boost.Asio coroutines. The constraints most easily tripped over:

- **Coroutines are not serialization.** Two coroutines spawned onto the same strand still
  interleave at every `co_await`. strand guarantees "never simultaneously", not
  "never interleaved". Invariants that span an `co_await` must be protected by a mutex or
  atomic; strand alone is not enough.
- **Socket I/O is serialized by strand, not by mutex.** Every `async_read` / `async_write`
  entry point hops onto the target strand first and holds no mutex during the socket
  operation. Re-snapshot `stream_` before each operation: a concurrent `async_close()` only
  makes in-flight operations return an error code, never a null dereference.
- **At most one request in flight per client** (single-flight). This is not enforced at
  runtime; violating it interleaves reads and writes on the same socket, which is undefined.
- **Routes are configuration-time only.** `httplib::server::router` has no internal locking;
  registering routes while serving is undefined behavior.
- **Scalar configuration is changeable at runtime**: always via `std::atomic`; pointer-shaped
  configuration via `std::atomic<std::shared_ptr<T>>` snapshots.

For the full picture (per-component strand topology, `sessions_` ownership, the shutdown
sequence that makes `router_.reset()` lock-free, and the list of APIs that require an idle
connection) see [THREAD_MODEL.md](THREAD_MODEL.md).

## Client Features

```cpp
// Async request shorthands
auto r1 = co_await client.async_get("/api/data");
auto r2 = co_await client.async_post("/api/json", boost::json::value{{"ok", true}});
auto r3 = co_await client.async_put("/api/data", "text", "text/plain");
auto r4 = co_await client.async_del("/api/data/1");
auto r5 = co_await client.async_options("/api/data");

// Request as an object (method + target + headers + body)
client::request req{httplib::method::post, "/upload"};
req.set(httplib::field::content_type, "application/json");
req.set_body(boost::json::value{{"k", "v"}});
auto r6 = co_await client.async_send_request(req);

// Lazy request: stream the request body yourself
auto lr = client.create_lazy_request();
co_await lr->write_header(httplib::method::post, "/stream", headers,
                          client::lazy_request::mode::chunked);
co_await lr->write_body(net::buffer(chunk), /*more=*/true);
auto lresp = co_await lr->read_response_lazy();     // body read later

// Lazy response: read the body on demand
auto resp = co_await client.async_send_request(req, client::http_client::body_mode::lazy);
if (resp) {
    auto body = co_await resp.value().read_string();   // result<std::string>
    // or stream it straight to a file:
    // co_await resp.value().read_to_file("download.bin");
}

// File download to disk (auto chunked + content-encoding)
co_await client.async_download(httplib::method::get, "/files/large.bin", "local_copy.bin");

// Timeout and redirect policies
client.set_timeout(std::chrono::seconds(5));
client.set_timeout_policy(client::http_client::timeout_policy::overall);   // or step / never
client.set_max_redirects(5);         // 0 = do not follow redirects
client.set_download_rate_limit(1024 * 1024);
```

### Downloader

```cpp
#include <httplib/client/downloader.hpp>

client::downloader dl(ex, pool);   // pool is shared_ptr<client::http_client_pool>
dl.set_config({ .resume = true });
dl.set_progress_callback([](client::downloader::progress_info const& p) { /* ... */ });
co_await dl.async_download("https://example.com/big.iso", "big.iso");
```

### Download Scheduler

```cpp
#include <httplib/client/download_scheduler.hpp>

client::download_scheduler sched(ex, pool, { .max_concurrent = 8 });
auto id = sched.add("https://example.com/a.bin", "a.bin");
auto st = co_await sched.async_wait_one(id);
co_await sched.async_run();   // drive all tasks to completion
```

### Disk Cache

```cpp
#include <httplib/client/disk_cache.hpp>

auto cache = std::make_shared<client::disk_cache>("/tmp/httplib-cache");
cache->set_max_size(512ull * 1024 * 1024);
cache->set_max_age(std::chrono::hours(24));
dl.set_cache(cache);
```

### Proxy Client (raw tunnel)

```cpp
#include <httplib/client/proxy_client.hpp>

client::proxy_client pc(ex, "proxy.internal", 3128);
boost::system::error_code ec;
co_await pc.async_connect("example.com:443", {}, ec);
```

## Database

Enable with `-DHTTPLIB_ENABLED_DATABASE=ON`. Backends: `"sqlite"`, `"mysql"`, `"odbc"`.

```cpp
#include <httplib/db/session.hpp>
#include <httplib/db/config.hpp>
#include <httplib/db/binder.hpp>
#include <httplib/db/extractor.hpp>
#include <httplib/db/connection_pool.hpp>

namespace db = httplib::db;

// Connect with a backend name + connection string
auto sess = co_await db::session::connect(ex, "sqlite", "db=:memory:");
// MySQL: "host=127.0.0.1 port=3306 user=root password=... db=main"
// Configs: db::mysql_config / db::sqlite_config / db::odbc_config, each with to_connection_string()

co_await sess.query("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, n INTEGER)");

// Parameterized query with named binds; NULL via nullptr / std::nullopt
co_await sess.query("INSERT INTO t VALUES (:id, :name, :n)",
                    db::bind("id", 1),
                    db::bind("name", std::string("alice")),
                    db::bind("n", 42));

// Read rows
auto r = co_await sess.query("SELECT id, name, n FROM t ORDER BY id");
for (std::size_t i = 0; i < r.row_count(); ++i) {
    auto id   = r[i].as_int64("id");     // std::optional
    auto name = r[i].as_string("name");
}

// Extract directly into variables (vector = all rows, optional = first row, scalar = first row)
std::vector<std::string> names;
co_await sess.query("SELECT name FROM t ORDER BY id", db::into(names));

// Prepared statements (created from the session, reused via the session's LRU cache)
auto stmt = sess.stmt("INSERT INTO t VALUES (:id, :name, :n)");
co_await stmt.bind("id", 2).bind("name", "bob").bind("n", 7).execute();

// Transactions
co_await sess.with_transaction([](db::session& s) -> net::awaitable<void> {
    co_await s.query("INSERT INTO t VALUES (3, 'carol', 9)");
});

// Connection pool (hand out sessions; returned on handle destruction)
db::pool_params p;
p.min_connections = 2;
p.max_connections = 8;
auto pool = std::make_shared<db::connection_pool>(
    db::make_pool(ex, "sqlite", "db=app.db", p));
auto handle = co_await pool->async_acquire();
co_await handle->query("SELECT 1");
```

`sess.set_query_logger(cb)` receives a `db::query_log_entry` per statement. `db_middleware` binds a `db::connection_pool` to each request and optionally wraps handlers in a transaction; `db_query_log_middleware` aggregates per-request query logs:

```cpp
#include <httplib/server/middleware/db_query_log.hpp>

router.set_http_handler<method::get>("/api/users", handler,
    middleware::db_middleware(pool, { .auto_transaction = true }),
    middleware::db_query_log_middleware(middleware::query_log_options{
        .on_request_complete = [](server::request const& req, auto const& entries) { /* ... */ },
        .slow_query_threshold = std::chrono::milliseconds(200),
        .on_slow_query = [](db::query_log_entry const& e) { /* ... */ } }));
```

Errors are thrown as `db::db_exception` carrying a `boost::system::error_code`.

## URL Utilities

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

## Examples

All examples are built when `HTTPLIB_ENABLED_EXAMPLES=ON` (default for root builds):

- `examples/demo` - server + client + WebSocket demo covering routing, middleware, streaming, SSE/NDJSON and the connection pool
- `examples/download_demo` - downloader / download scheduler demo
- `examples/stress_test` - wrk-like benchmark (`--url`, `-c`, `-d`, `-X`, `--body`)

## License

This project is distributed under the Boost Software License, Version 1.0. See `LICENSE`.
