# 重构计划：收敛 server::request 与 client::response（含 client::request）的 body 逻辑

> 状态：草案
> 目标：消除 inbound/outbound 三处 body 状态、访问器与读取逻辑的重复，方向维度只保留一处
> 前置约束：不要求保持现有公开接口 / ABI 兼容（可改签名）
> 关联代码：
> - `lib/body/lazy_body_reader.hpp`（现有 CRTP 读取器）
> - `lib/server/request_impl.hpp`、`lib/client/response_impl.h`、`lib/client/request_impl.h`
> - `lib/server/request.cpp`、`lib/client/response.cpp`、`lib/client/request.cpp`
> - `lib/body/any_body.hpp`

---

## 1. 背景

当前存在三份 body 相关实现：

| 类型 | 角色 | body 生命周期 | 存储/所有权 |
|---|---|---|---|
| `server::request` | 入站请求 | lazy（可流式/物化） | `unique_ptr<impl>` |
| `client::response` | 入站响应 | lazy（可流式/物化） | `shared_ptr<impl>` |
| `client::request` | 出站请求 | eager（用户设置） | `shared_ptr<impl>` |

`lazy_body_reader<IsRequest, Derived>` 已经收敛了一部分（parser 状态、`read_mutex_`、`read_body` 循环、`read_some_raw/decompressed`），但仍存在以下重复。

---

## 2. 根因

四个正交关注点被耦合在一起，导致只能用 CRTP + `friend` 硬拼，并残留三份拷贝：

1. **消息方向**（request / response）
2. **body 生命周期**（lazy / streaming / materialized）
3. **错误策略**（server 抛 `system_error` / client 返回 `boost::system::result`）
4. **所有权**（`unique_ptr` / `shared_ptr`）

两个关键事实可利用：

- **物化后真正需要的只有 `any_body::value_type`**，整只 `http::request<any_body>` / `http::response<any_body>` 只是为了满足 beast parser 的 `release()`；header 已单独存储。
- **方向只影响两件事**：parser 类型、header 类型。收敛到 traits 后其余全部方向无关。

### 2.1 重复清单（含位置）

**A. impl 层：`optional<msg_>` + eager 访问器**
- `lib/server/request_impl.hpp:128-251` vs `lib/client/response_impl.h:79-135`
- `as_string/as_json/as_form_data/as_query_params`、`take_body<T>`、`is_body_done`、`store_body`、若干 `is_*`
- 差异仅：所有权、数据源方法名（`read_some` vs `async_read_some`）、client 的 `store_body` 额外清 `parent_->read_impl_`。

**B. public 层：`read_*` 包装**
- `lib/server/request.cpp:189-305` vs `lib/client/response.cpp:111-233`
- 4 个 typed read：唯一实质差异是错误策略，读 body 的 lambda 完全一致。
- `read_some_raw` / `read_some_decompressed` 各 2 个重载。
- `read_body` 语义不一致：server 自动按 content-type 派发且抛异常；client 传 `nullptr` 且返回 `error_code`。

**C. `client::request` 第三份**
- `lib/client/request.cpp:170-227`：`as_*` / `is_*`（多 `is_file`），直接操作 `body()`。

**D. 疑似死分支**
- `lib/server/request_impl.hpp:298-303` 以 `reader_ == nullptr` 判定"已物化"，但 `request::impl::make_request` 恒传入 task、构造函数恒给 `reader_` 赋值；session 是显式 `read_body()` 先物化。该分支当前实际不可达，收敛时顺带清除。

---

## 3. 目标架构

三个正交组件，方向只在 `body_reader` 的 `IsRequest` 参数体现：

```
body_state                       物化 body 值 + as/is/take（方向无关，零依赖）
body_reader<IsRequest, Source>   parser 状态机 + 异步读取 + 串行化（方向无关，数据源注入）
message impl = header + body_reader<...> + 方向专有元数据
```

- `server::request::impl`：`header_` + `body_reader<true, session::http_task>` + 路由元数据。
- `client::response::impl`：`header_` + `body_reader<false, response::impl>` + status/content-length + sse/ndjson。
- `client::request::impl`：只需要 `body_state` 的访问器语义，不接 reader。

`read_*` 只有一份 canonical 实现（返回 `result`），leaf 决定是否再抛。

---

## 4. 组件接口草案

### 4.1 方向类型（内联于 `body_reader`，无独立 traits）

Beast 的 `parser` / `message` 本身就以 `isRequest` 为模板参数，直接用即可，无需 `select`/`direction_traits`：

```cpp
// lib/body/body_reader.hpp 内
using header_parser_t = http::parser<IsRequest, http::empty_body>;
using raw_parser_t    = http::parser<IsRequest, http::buffer_body>;
using any_parser_t    = http::parser<IsRequest, body::any_body>;
using message_t       = http::message<IsRequest, body::any_body>;
```


### 4.2 `body_state`（`lib/body/body_state.hpp`，新）

```cpp
class body_state {
public:
    bool ready() const;

    template <typename Body> bool is() const;      // body_ && is_body_type<Body>()
    template <typename T>   T    take();            // 移动取出并 reset

    std::string const&        as_string() const;
    boost::json::value const& as_json() const;
    html::form_data const&    as_form_data() const;
    html::query_params const& as_query_params() const;

    void assign(body::any_body::value_type v);
private:
    std::optional<body::any_body::value_type> body_;   // 不存整只 message
};
```

三个 impl 共用 → 删除 3 份 `as_*` / `is_*`。

### 4.3 `body_reader<IsRequest, Source>`（`lib/body/body_reader.hpp`，新；替代 `lazy_body_reader`）

```cpp
template <bool IsRequest, typename Source>
class body_reader {
public:
    using header_parser_t = http::parser<IsRequest, http::empty_body>;
    using raw_parser_t    = http::parser<IsRequest, http::buffer_body>;
    using any_parser_t    = http::parser<IsRequest, body::any_body>;
    using message_t       = http::message<IsRequest, body::any_body>;
    using body_setup_fn   = std::function<void(message_t&)>;

    void start(Source* src,
               std::unique_ptr<header_parser_t> hp,
               std::uint64_t body_limit,
               net::any_io_executor ex,
               std::function<void()> on_stored = {});

    net::awaitable<void>        read_body(body_setup_fn const& setup, boost::system::error_code& ec);
    net::awaitable<std::size_t> read_some_raw(net::mutable_buffer const&, boost::system::error_code& ec);
    net::awaitable<std::size_t> read_some_decompressed(net::mutable_buffer const&, boost::system::error_code& ec);
    bool is_body_done() const;

    body_state& state();
private:
    template <typename P>
    net::awaitable<void> pull(P& p, boost::system::error_code& ec)
    { co_await source_->read_some(p, ec); }

    Source* source_ = nullptr;
    std::unique_ptr<util::async_mutex> read_mutex_;
    std::unique_ptr<header_parser_t> header_parser_;
    std::unique_ptr<raw_parser_t> raw_parser_;
    std::unique_ptr<any_parser_t> any_parser_;
    std::uint64_t body_limit_ = 0;
    std::function<void()> on_stored_;
    body_state state_;
};
```

相对现状的变化：
- 去掉 CRTP 与 `friend`；构造注入 `Source`。
- `read_some` / `store_body` / `reader_is_materialized` 三个派生钩子消失。
- `store_body` 变为 `state_.assign(std::move(msg.body()))`。
- 副作用（client 清 `parent_->read_impl_`）改为构造期传入的 sink 回调，不再用虚函数/CRTP。

**Source 适配**：server 的 `session::http_task::read_some` 已符合命名；client 的 `http_client::impl::async_read_some` 需改名或在 client 侧包一个 7 行 adapter（`read_some` 转发到 `async_read_some`）。

### 4.4 唯一 typed 读取 + 错误适配（`lib/body/read.hpp`，新）

```cpp
template <typename Body, typename R>
net::awaitable<boost::system::result<typename Body::value_type>>
read_as(R& r, body_setup_fn setup = {}, boost::system::error_code* out = nullptr) {
    boost::system::error_code ec;
    co_await r.read_body(setup ? setup
                 : [](typename R::message_t& m){ m.body() = typename Body::value_type{}; }, ec);
    if (out) *out = ec;
    if (ec) co_return ec;
    co_return r.state().template take<typename Body::value_type>();
}
```

leaf 用法：

```cpp
// client::response：直传
net::awaitable<result<std::string>> response::read_string()
{ co_return co_await read_as<body::string_body>(body_); }

// server::request：leaf 决定抛
net::awaitable<std::string> request::read_string() {
    auto r = co_await read_as<body::string_body>(body_);
    if (!r) throw boost::system::system_error(r.error());
    co_return std::move(*r);
}
```

`read_body` 统一返回 `awaitable<boost::system::error_code>`；server 的 multipart 自动派发只是换 setup 回调。

---

## 5. 任务分解

### Phase 0：准备
- [x] 方向类型映射直接内联到 `body_reader`（`std::conditional_t<IsRequest, Req, Resp>`），不单独抽 `direction_traits` 文件。
- [x] 确认 `body_setup_fn`、`util::async_mutex`、`net::any_io_executor` 等现有依赖可直接复用。
- [x] 确认 `http_client::impl::async_read_some`、`session::http_task::read_some` 签名差异及 adapter 方案。

### Phase 1：`body_state` 抽取（无行为变化）
- [x] 新建 `lib/body/body_state.hpp` + `.cpp`。
- [x] 把 `as_string/as_json/as_form_data/as_query_params`、`is_*`、`take_body` 从两个 inbound impl 迁入。
- [x] `server::request::impl`、`client::response::impl` 改为持有 `body_state`。
- [x] `client::request` 的 `as_*` / `is_*` 复用 `body_state` 语义（改用 `body::access` free helper）。
- [x] 删除 `lib/server/request_impl.hpp:128-251`、`lib/client/response_impl.h:79-135`、`lib/client/request.cpp:170-227` 的对应实现。
- 验收：`ctest -C Debug` 全绿（6/6，含 core/http/client/proxy/jwt/db）；public 行为不变。

> 落地说明：
> - `body_state` 存 `std::optional<any_body::value_type>`；`take()` 取出后 reset（比原实现多一次清理，验证无回归）。
> - `body::access`（`is<Body>` / `as_*` / `take<T>`）是方向无关的访问原语，三个类共用；`client::request` 直接作用于 `impl_->body()`。
> - `server::request::impl::reader_is_materialized()` 原为恒假的 `reader_ == nullptr`，改为 `body_state_.ready()`（等价且更正确）。
> - 顺带修正 `lazy_body_reader.hpp` 中引用旧 `msg_` 的注释。

### Phase 2：`body_reader` 去 CRTP
- [x] 新建 `lib/body/body_reader.hpp`；内部持 `body_state`。
- [x] 数据源以 `Source` 注入（server 用 `session::http_task`，client 用 `response::impl` 自身，见落地说明）。
- [x] `server::request::impl` 改为 `body_reader<true, session::http_task>` 组合。
- [x] `client::response::impl` 改为 `body_reader<false, response::impl>` 组合；清 `read_impl_` 改 sink 回调。
- [x] 删除两处 friend 声明与 `reader_is_materialized` / `store_body` 钩子。
- [x] 顺带移除 `request_impl.hpp` 的不可达 `reader_ == nullptr` 分支（改 `body_state_.ready()`，Phase 1 已完成）。
- 验收：`ctest -C Debug` 全绿（6/6）。

> 落地说明：
> - `body_reader<IsRequest, Source>` 定义在 `httplib::detail`；方向仅由模板参数 `IsRequest` 决定（内部 `std::conditional_t`），不额外抽 traits。
> - 未引入形式化 `concept`，Source 为结构化要求（提供 `read_some(parser, ec)`）；后续补单测时再考虑加约束。
> - **client 的 Source 改为 `response::impl` 自身**而非 `http_client::impl`：`http_client::impl` 是私有嵌套类，只有 `response::impl` 是 friend，独立 adapter 类无法命名它。`response::impl::read_some` 仅转发到 `parent_->async_read_some`。
> - `lazy_body_reader.hpp` 已在 Phase 3 删除（此前暂留作回退）。

### Phase 3：typed 读取收敛
- [x] 新建 `lib/body/read.hpp::read_as`。
- [x] `request` / `response` 的 `read_string/json/form_data/query_params` 全部改走 `read_as`。
- [x] `read_body` 统一返回 `error_code`（server 的 `read_body(ec)` 重载合并为 `read_body()->awaitable<error_code>`）；自动派发保留为 setup 回调。
- [x] `response::read_to_file` 改用 `read_as<file_body>` 的 file_body setup。
- [x] 删除 `lib/body/lazy_body_reader.hpp` 及 impl 中残留的 `take_body` 转发。
- 验收：`ctest -C Debug` 全绿（6/6）。

> 落地说明：
> - `read_as<Body>(reader, setup = {})` 是唯一 typed 读取实现，要求 Reader 提供 `message_t` / `body_setup_fn` / `read_body(setup, ec)` / `body_state()`。
> - 错误策略仍在 leaf：client 直接 `co_return` result；server `if (!result) throw`。
> - `read_body` 签名变更属公开 API 破坏（已获授权）；`session.cpp` 与文档头部同步更新。
> - `impl::take_body` 已无调用方，删除；取出统一走 `body_state().take<T>()`。

### Phase 4：进一步统一
- [x] `body_reader` 用 fake `Source` 补单测（`tests/body_reader_test.cpp`，覆盖物化 / raw 流式 / 解压流式）。
- [ ] 单 parser 状态机：`parser<any_body>` + 运行时 `decompress` 开关，合并 `raw_` / `any_`。（**暂缓**）
- [ ] `header_access<Header>` mixin，消除 `operator[]/at/has/base` 重复。（**暂缓**）
- [ ] 公共 API 模板化 `basic_message<IsRequest>` + `requires` 分方向（若接受去掉 PIMPL）。（**暂缓**）
- 验收：`ctest -C Debug` 全绿（6/6）。

> 落地说明 / 暂缓理由：
> - 单测用 `boost::beast::test::stream` 构造假 `Source`，只经由 `read_some(parser, ec)` 交互，无需真实 socket；恰好验证 Phase 2 去 CRTP 的核心收益。
> - 单 parser 状态机需给 `any_body::reader` 增加"是否解压"开关，触及公共 body 模型，收益（少一个 parser 成员）不抵风险。
> - `header_access` 仅消除 4 个一行转发，收益低，且会把模板混入公开类。
> - `basic_message<IsRequest>` 需去掉 PIMPL、改公开类定义，属独立的大改动，不应夹带在本次重构中。

---

## 6. 验证

构建与测试（见 `AGENTS.md`）：

```bash
mkdir build && cd build
cmake .. -DHTTPLIB_ENABLED_TESTS=ON
cmake --build .
ctest --test-dir build -C Debug            # 全量
ctest --test-dir build -C Debug -L core|http|client|proxy   # 分模块
```

- 每阶段结束必须全绿；Phase 1/2 不得改变可观测行为。
- 新增：`body_reader` 的 fake-Source 单测（Phase 4）。
- 建议覆盖点：空 body、Content-Length、chunked、gzip/br 解压、body_limit 溢出、并发 read（read_mutex 串行化）、streaming 后拒绝物化、物化后 `is_*` 判定、abort/连接关闭。
- 格式化：`clang-format`（WebKit base，IndentWidth 4，ColumnLimit 100）。

---

## 7. 风险与回退

| 风险 | 说明 | 缓解 |
|---|---|---|
| Source 命名不统一 | server `read_some` vs client `async_read_some` | client 侧 adapter，不动现有 impl 公共方法 |
| 生命周期 | `body_reader` 持有 `shared_ptr<Source>`，server `http_task` 本身是 `shared_ptr` | 保持强引用；检查 `~http_task` 与 abort 路径 |
| `take()` reset 语义 | 现状 take 后 `is_*` 仍为真 | 明确 reset；补测试确认调用方不依赖旧行为 |
| 错误策略改动 | 统一 `result` 是公开 API 破坏性变更 | 本计划默认接受；如需保留，仅 leaf 加抛/`result` 双入口 |
| 解压 limit / pending 语义 | `read_some_decompressed` 溢出到 `buffer_body::pending` | 迁移时保持逐行等价，先补测试再删旧代码 |

回退：每 Phase 独立提交；Phase 2 保留旧 `lazy_body_reader.hpp` 直到 Phase 3 验收通过再删。

---

## 8. 预期收益

- 方向维度只在 `body_reader` 的 `IsRequest` 出现一次；删除两处 `#if constexpr (IsRequest)` 与 friend 体系。
- `body_state` 一份，三处 `as_*` / `is_*` 拷贝清零。
- 存储由 `optional<message>` 缩为 `optional<any_body::value_type>`。
- `body_reader` 可通过 fake `Source` 单测，摆脱"必须真起连接才能测 body 读取"。
- 错误策略与读取实现解耦：实现只保留 `result` 版本，抛异常是 leaf 的三行便利层。
