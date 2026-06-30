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

## Test

```bash
ctest --test-dir build          # or run build/bin/x64/Debug/httplib_tests directly
```

Tests use Catch2 via `Catch2::Catch2WithMain` (auto-generated main). Each test spins up a real server on `127.0.0.1:0` and hits it with a client over TCP — no mocks. Tests must link `httplib` + `Catch2::Catch2WithMain` and include the internal lib dir (`PRIVATE ${HTTPLIB_LIB_DIR}`) for access to impl headers.

## Architecture

- **Public API**: `include/httplib/` — classes with PIMPL, designed to hide Boost types from callers.
- **Implementation**: `lib/` — `*_impl.h`/`*_impl.cpp` files. They include the public headers + Boost internals.
- **Namespace aliases** (from `config.hpp`): `net = boost::asio`, `http = beast::http`, `websocket = beast::websocket`, `ssl = boost::asio::ssl`, `fs = std::filesystem`. Always use these aliases; never write `boost::asio` directly.
- **Router**: `server::router` is an abstract base; real implementation is `server::router_impl` in `lib/server/router_impl.h`. Template methods live in `include/httplib/server/router.inl`.
- **Server**: `server::http_server` holds a shared_ptr to `http_server::impl` (PIMPL). Start with `listen()` + `async_run()`.
- **Middleware**: Per-route, passed as trailing variadic args to `set_http_handler`. Each middleware must have `bool before(request&, response&)` and optionally `bool after(request&, response&)`. Return `false` to short-circuit (no handler run).
- **Body types**: `any_body::value_type` variant. Access via `req.body().as<body::string_body>()`, etc. Body types: `string_body`, `json_body`, `form_data_body`, `query_params_body`, `empty_body`, `file_body`.
- **Examples**: `examples/demo/` (server + client demo) and `examples/stress_test/` (wrk-like benchmark). Both auto-built via `GLOB_RECURSE` within their own `CMakeLists.txt` when `HTTPLIB_ENABLED_EXAMPLES=ON`. Stress test additionally links `Boost::program_options`.

## Style

- `.clang-format` exists — WebKit base, IndentWidth 4, ColumnLimit 100, BraceWrapping after functions/classes/structs.
- No `.github` CI (gitignored). Format manually or via clang-format before commit.
- MSVC requires `/bigobj` compile option (set in CMake for the library and tests).

## Optional features

- `HTTPLIB_ENABLED_SSL` macro gates OpenSSL code. Check `#ifdef HTTPLIB_ENABLED_SSL` before adding SSL-dependent code.
- `HTTPLIB_ENABLED_COMPRESS` macro gates Brotli compression.
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
