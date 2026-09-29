# AGENTS.md

## Build

```bash
mkdir build && cd build
cmake .. -DHTTPLIB_ENABLED_TESTS=ON
cmake --build .
```

- Build output goes to `bin/x64/[Debug|Release]/` on 64-bit, `bin/x86/` on 32-bit.
- Add `-DHTTPLIB_ENABLED_SSL=ON` for HTTPS/WSS (requires OpenSSL).
- Add `-DHTTPLIB_ENABLED_COMPRESS=ON` for Brotli (requires Boost.Iostreams + unofficial-brotli).
- Add `-DHTTPLIB_ENABLED_DATABASE=ON` for the DB module (requires Boost.MySQL 1.85+ / Boost.Charconv, SQLite3, and ODBC).

## Test

```bash
ctest --test-dir build -C Debug   # add -L core|http|client|proxy|jwt|db to filter
```

Tests use Catch2 via `Catch2::Catch2WithMain` (auto-generated main). The server/client/proxy tests spin up a real server on `127.0.0.1:0` and hit it over TCP — no mocks. Tests are split into 6 executables/targets (`httplib_test_core`, `_http`, `_client`, `_proxy`, `_jwt`, `_db`), each registered with a ctest label. New tests link the shared `httplib_test_support` INTERFACE target (carries `httplib`, `Catch2::Catch2WithMain`, OpenSSL, `/bigobj`, and the internal `${HTTPLIB_LIB_DIR}` include) via the `add_httplib_test(<target> <label> <sources...>)` helper, so no per-target boilerplate is needed.

## Architecture

- **Public API**: `include/httplib/` — classes with PIMPL, designed to hide Boost types from callers.
- **Implementation**: `lib/` — `*_impl.h`/`*_impl.cpp` files. They include the public headers + Boost internals.
- **Namespace aliases**: the public `config.hpp` exports only `net = boost::asio`, `ssl = boost::asio::ssl`, `tcp = net::ip::tcp`, `fs = std::filesystem` — the public surface must not name Beast types. The Beast aliases `beast = boost::beast`, `http = beast::http`, `websocket = beast::websocket` live in the private `lib/beast_alias.hpp`; every `lib/` file that uses `http::`/`websocket::`/`beast::` must include it. Use `net::`, never `boost::asio`, in public headers.
- **Router**: `server::router` is an abstract base; real implementation is `server::router_impl` in `lib/server/router_impl.h`. Template methods live in `include/httplib/server/router.inl`.
- **Server**: `server::http_server` holds a shared_ptr to `http_server::impl` (PIMPL). Start with `listen()` + `run()`/`async_run(ec)`.
- **Middleware**: Per-route aspects, passed as trailing variadic args to `set_http_handler`, or globally via `router::use()`. Each aspect may provide `before(request&, response&)` and/or `after(request&, response&)`, returning `bool` or `net::awaitable<bool>`. Return `false` from `before` to short-circuit (handler + `after` skipped). WebSocket handlers take open/message/close callbacks and do not accept aspects.
- **Body types**: a runtime `body_state` (a `std::variant` of the body kinds, replaced the old `any_body::value_type`) selected by `Content-Type`. Public access is via typed accessors `as_string()` / `as_json()` / `as_form_data()` / `as_query_params()`; `type()` returns the `httplib::body_type` enum (`none`, `empty`, `string`, `json`, `query_params`, `form_data`).
- **Examples**: `examples/demo/` (server + client + ws), `examples/download_demo/` (downloader/scheduler) and `examples/stress_test/` (wrk-like benchmark). All auto-built via `GLOB_RECURSE` within their own `CMakeLists.txt` when `HTTPLIB_ENABLED_EXAMPLES=ON`; stress test additionally links `Boost::program_options`.
- **Threading**: full strand/mutex topology is documented in `THREAD_MODEL.md`. Read it before touching socket I/O, session lifetime, or the client connection pool.

## Threading invariants

Violating any of these introduces a data race or undefined behavior. `THREAD_MODEL.md` has the full picture; these are the ones that are easy to break by accident:

- **strand serializes, it does not sequence.** Two coroutines on the same strand still interleave at every `co_await`. Anything that must hold across an `co_await` needs a mutex or atomic — not just a strand.
- **Socket I/O takes no mutex.** Every `async_read`/`async_write` entry point hops onto the target strand and holds nothing during the operation. Never add a lock around a socket op; fix ordering with the strand instead.
- **Re-snapshot the stream before every operation.** Read `stream_->load()` immediately before use. A concurrent `async_close()` exchanging it to nullptr is legal and only makes the in-flight op fail with an error code.
- **Never touch another object's socket from the calling thread.** `session::abort()` and `http_client::async_close()` both `post`/`dispatch` back to the owning strand first. Copy that pattern.
- **Session registration must precede `co_spawn`.** `co_accept()` inserts into `sessions_` synchronously on `strand_` before spawning `conn->run()`. Reordering breaks the shutdown drain, which is what makes `router_.reset()` safe without a lock.
- **Tear down state before closing handles.** In pool `on_stop()`, move connections out and clear pool bookkeeping *first*, then `close()` them — otherwise close-time callbacks re-enter live pool state.
- **Router has no internal locking.** Registration is configuration-time only (before `run()`). Do not add a lock; document the contract instead. `reset()` runs on the strand after all sessions drain.
- **Single-flight per client.** At most one request in flight per `http_client`; not enforced at runtime. The pool checks `has_active_session()` before reuse.
- **APIs needing an idle connection**: `is_alive()`/`async_is_alive()` (sync `MSG_PEEK`), `is_open()`, and the rate-limit setters. These are documented as such in the public headers — keep those notes accurate if the implementation changes.

## Style

- `.clang-format` exists — Microsoft base, `BreakBeforeBraces: Allman` (with `AfterClass: false`), `NamespaceIndentation: All`, `PointerAlignment: Left`, IndentWidth 4 (default). ColumnLimit is not overridden (Microsoft default 120).
- No `.github` CI (gitignored). Format manually or via clang-format before commit.
- MSVC requires `/bigobj` (set in CMake for the library, tests and every example).

## Optional features

- `HTTPLIB_ENABLED_SSL` macro gates OpenSSL code. Check `#ifdef HTTPLIB_ENABLED_SSL` before adding SSL-dependent code.
- `HTTPLIB_ENABLED_COMPRESS` macro gates Brotli compression.
- `HTTPLIB_ENABLED_DATABASE` macro gates the DB module (`include/httplib/db/*`, `middleware/db_middleware.hpp`, `middleware/db_query_log.hpp`); include `#ifdef HTTPLIB_ENABLED_DATABASE` guards.
- `HTTPLIB_ENABLED_UNITY_BUILD` turns on CMAKE_UNITY_BUILD for the library; tests stay TU-isolated. `HTTPLIB_ENABLED_EXAMPLES` / `HTTPLIB_ENABLED_TESTS` default ON for root builds.
- `HTTPLIB_SHARED_LIBRARY` switches library type; `HTTPLIB_API` controls dllimport/dllexport on Windows.

## Stress Test

```bash
cmake --build . --target stress_test
# Run against a server:
./bin/x64/Debug/stress_test.exe --url http://127.0.0.1:8080/api/echo-json -X POST --body '{"msg":"test"}' -c 100 -d 10
```

- Built as part of examples (requires `-DHTTPLIB_ENABLED_EXAMPLES=ON`, the default for root builds).
- Uses `net::thread_pool` + per-connection `co_spawn` for concurrent async requests.
- Reports wrk-style: Thread Stats (avg/stdev/max), Latency Distribution (p50/p75/p90/p99), Status Codes, Req/Sec, Transfer/Sec.
- `--url` is the only target option; `--host`/`--port`/`--path` are replaced.
- Default method is GET. Use `-X POST --body '...'` for POST.
