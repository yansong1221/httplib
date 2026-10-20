# httplib 设计审查报告

> 日期：2026-10-20
> 范围：`include/httplib/`（公共 API）与 `lib/`（实现）全量代码审查；`examples/`、`tests/` 仅作佐证引用。
> 方法：三路并行深度分析（server / client / body-db-util-middleware），关键高危结论逐一对照源码复核。
> 说明：以下每条问题均附文件:行号证据。个别条目在复核时被修正（见 §13 勘误），已在正文标注。

---

## 目录

- [总体评价](#总体评价)
- [一、安全问题（P0）](#一安全问题p0)
- [二、正确性 / 生命周期缺陷](#二正确性--生命周期缺陷)
- [三、错误模型不统一（最大面积的设计债）](#三错误模型不统一最大面积的设计债)
- [四、性能热点](#四性能热点)
- [五、静默配置陷阱](#五静默配置陷阱)
- [六、API 一致性杂项](#六api-一致性杂项)
- [七、勘误](#七勘误)
- [八、优先级总表](#八优先级总表)

---

## 总体评价

库的整体架构方向是健康的：PIMPL 隐藏 Boost 类型、明确的 strand/mutex 拓扑（THREAD_MODEL.md）、
配置期/运行期 API 分区文档、ctest 六目标 + 特性门控测试。

但存在三类**系统性**设计缺口，几乎贯穿所有模块：

1. **错误模型不统一** —— 同一操作家族里混用
   「抛异常 / 返回 `error_code`（出参）/ 返回 `boost::system::result` / 返回哨兵值」四种约定，
   调用方无法写出一致的错误处理代码。
2. **生命周期不编码** —— 借用视图、裸指针跨层持有，生命周期约束靠注释而非类型/运行期断言表达。
3. **静默降级（fail-open)** —— 配置错误或异常路径悄悄落到默认行为；安全相关 API（JWT、CORS、
   反向代理、WS 升级）也存在同样的 fail-open 缺省值。

---

## 一、安全问题（P0）

### 1.1 JWT verifier 默认不校验签名（fail-open）⚠️【高】

**证据**：`lib/jwt.cpp:562-579`（旧实现，文件现已拆分为 `lib/jwt/`）

```cpp
void verifier::verify(decoded_jwt const& jwt, boost::system::error_code& ec) const
{
    ec.clear();
    if (alg_)                       // ← 算法比较 + 签名校验整体包在这个 if 里
    {
        auto jwt_alg = jwt.get_algorithm();
        if (jwt_alg != alg_->name()) { ec = ...algorithm_mismatch; return; }
        if (!jwt.verify(*alg_, ec)) { return; }
    }
    // alg_ 为空时直接跌进 claims 校验（exp/iss/nbf 均为明文 JSON）
}
```

**为什么不合理**：默认构造的 verifier（未调用 `allow_algorithm()`）只校验 claims 就放行，
而 claims 任何人都能改写。安全 API 的默认值必须 fail-closed。

**建议**：`verify()` 开头加 `if (!alg_) { ec = make_error_code(error::not_initialized); return; }`；
或把 `jwt.hpp:276-282` 已鼓励的 `verify(algorithm const&)` 工厂变成唯一入口，删除无参路径。

> **✅ 已修复（2026-10）**：jwt 模块整体改为委托 jwt-cpp（模块拆分为 `include/httplib/jwt/`
> + `lib/jwt/`，分工边界见 `lib/jwt/engine.hpp` 与 `lib/jwt/error_detail.hpp` 顶部注释）。
> verifier 改为 PIMPL + 算法白名单：允许列表为空 → `error::not_initialized`（新增枚举值）；
> token 声明的算法不在白名单 → `error::algorithm_mismatch`（与 jwt-cpp verifier 的
> wrong_algorithm 语义一致）。`allow_algorithm()` 同时从「覆盖」改为「追加」，支持多算法
> 轮换。fail-closed 白名单实现于 `lib/jwt/verifier.cpp` 的 verify() 入口。回归测试：
> `tests/jwt_p0_test.cpp`（[p0][jwt][regression]）。

### 1.2 `builder::sign()` 重置 header，`set_content_type`/`set_key_id` 静默丢失【高】

**证据**：`lib/jwt.cpp:464-476` 与 `:388-404`（旧实现，文件现已拆分为 `lib/jwt/`）

```cpp
std::string builder::sign(algorithm const& alg)
{
    header_ = { { claim::algorithm, alg.name() } };   // ← 整体覆盖 header_
    ...
}
builder& builder::set_key_id(std::string_view kid)
{ return set_header_claim(claim::key_id, ...); }      // ← 写入的就是 header_，随后被覆盖
```

**为什么不合理**：`sign()` 前调用 `set_header_claim()` 设置的任意 header claim（kid 在 JWKS
场景是必需的）被无声抹掉，生产环境签名方才知道。

**建议**：`sign()` 改为 `header_.emplace(claim::algorithm, alg.name())`（保留并覆盖语义），
并补测试：`create().set_key_id("k1").sign(hs256)` 的 token header 必须含 `kid`。

> **✅ 已修复（2026-10）**：`sign()` 改为对 alg/typ 两个结构性字段 `insert_or_assign`
> （重复 sign 允许换算法），其余 header claim 全部保留；公共头文件补了契约注释。
> 回归测试：`tests/jwt_p0_test.cpp`（kid/cty/自定义 claim 三断言 + typ 对照组）。

### 1.3 WebSocket 升级请求路由不命中时不回任何 HTTP 响应【高】

> **✅ 已修复（2026-10）**：路由判定前移到 session 分叉点——命中 WS 路由才劫持
> 转 `websocket_task`（`ws_handler_entry` 随构造移交，`run()` 不再二次查询）；
> 未命中的升级请求按普通 HTTP 请求路由，404/405 与中间件语义照常生效。
> 回归测试：`tests/p0_review_test.cpp` P0-3。

**证据**：`lib/server/session.cpp:277-283`、`lib/server/websocket_conn_impl.cpp:216-222`

```cpp
// session.cpp —— is_upgrade 为真，在路由匹配【之前】就转成 websocket_task
if (websocket::is_upgrade(get_impl(req).get())) {
    co_return std::make_shared<websocket_task>(...);
}
// websocket_conn_impl.cpp —— 找不到 handler 时直接结束
auto entry = server_impl_->router().query_ws_handler(req_);
if (!entry) { co_return; }          // TCP 关闭，无响应
```

**为什么不合理**：
- 违反 RFC 6455 §4.2（不能升级时应返回 4xx），浏览器端表现为晦涩的 connection closed 而非 404；
- 任何带 `Upgrade: websocket` 头的请求都被抢走路由语义，与「未匹配 → 404/405」的路由契约不一致。

**建议**：`query_ws_handler` 未命中时走普通 HTTP 路径回 404（`resp.set_error_content(404)`）。

### 1.4 流式响应中途失败不标脏连接 → keep-alive 协议 desync【高】

> **✅ 已修复（2026-10，handler 抛异常路径）**：`body_writer` 引入 header 上线事实
> 闩锁 `header_sent`（write_header 成功置位、永不回落），session/response 的判据
> 换底；`response_impl` 全部 body 写入入口在头上线后拒绝改写。中途失败 ⇒ catch
> 里 `keep_alive(false)` 标脏关闭（既有路径），`async_write` 经闩锁跳过，500 不会再
> 叠加进悬空 chunked body。
> 回归测试：`tests/p0_review_test.cpp` P0-4。

**证据链**：

| 位置 | 现象 |
|---|---|
| `lib/server/reverse_proxy_impl.cpp:394-403` | 上游读失败仅 `logger_->trace(); co_return;` |
| `lib/server/session.cpp:425-429` | `async_write` 返回时仅查 `keep_alive()` |
| `lib/server/response_impl.hpp:52` / `lib/body/body_writer.hpp:503-507` | `keep_alive` 只在构造置 true、仅写入失败才置 false；**上游读失败路径不会** |
| `lib/server/session.cpp:360-368` | header 已发送后 handler 再抛异常，catch 里 `set_error_content(500)` 写进的是永远发不出去的消息，且连接未标脏 |

**为什么不合理**：chunked / Content-Length 响应体没写完就复用连接读下一条请求，
客户端会把下一个响应头当作上一个响应 body 的延续 —— 跨连接协议错乱。proxy relay、
SSE/NDJSON、用户 stream_writer 所有流式路径共同缺口。

**建议**：body_writer/session 增加 `mark_broken()`：`begin_stream` 后任何中途提前退出
（错误、协程异常、上游失败） ⇒ `keep_alive(false)`；session 在 `async_write` 后补
「流式响应未自然收尾 ⇒ 强关连接」检查（如 `writer().streaming() && !writer().done()`）。

### 1.5 cors_middleware 多 origin 配置静默失效【中】

> **✅ 已修复（2026-10）**：静态 `origin_header()` 改为按请求 Origin 解析：白名单
> 命中回显该 Origin + Vary: Origin（按 comma token 判重追加）；凭据开启时通配
> 改回显请求 Origin，`"*"` 与 `ACAC:true` 不再同屏；未命中不回 ACAO。无 Origin
> 的请求保持既有行为（通配/单值按配置原样回应）。
> 回归测试：`tests/p0_review_test.cpp` P0-5。

**证据**：`lib/server/middleware/cors.cpp:34-55`

```cpp
std::string origin_header() const {
    if (origins_.empty()) return "*";
    if (origins_.size() == 1) return origins_[0];
    return {};                  // >1 个 origin → 空串 → apply() 跳过 ACAO
}
```

另：`allow_credentials(true)` 配合默认 `origins_ = {"*"}` 产生的
`Access-Control-Allow-Origin: *` + credentials 组合按 Fetch spec 无效。

**建议**：多 origin 时回显请求 Origin（校验白名单 + 追加 `Vary: Origin`）；
`*` 与 credentials 组合在配置阶段报错。

### 1.6 session id 使用 MT19937 随机源【中→低】

> **✅ 已修复（2026-10）**：`generate_id()` 改为整 id 由 boost 的 UUID 随机生成器
> 产出（`random_generator` 各实例独立接线随机源，生成器按线程实例化，无跨请求
> 可外推的状态）；id 作为 store 键与 cookie 值本身不透明，形状变化无外部依赖。
> 回归测试：`tests/middleware_test.cpp` session 系列。

**证据**：`lib/server/middleware/session_mw.cpp:402`

```cpp
static thread_local std::mt19937_64 rng(std::random_device {}());
```

**为什么不合理**：session id 是安全敏感值（cookie 承载），MT19937 状态可预测性有公开结论；
库在 SSL 构建下已依赖 OpenSSL。

**建议**：`#ifdef HTTPLIB_ENABLED_SSL` 用 `RAND_bytes` / `CRYPTO_rand_bytes`；无 SSL 时
逐 id 用 `std::random_device` 而非播种 MT。

---

## 二、正确性 / 生命周期缺陷

### 2.1 `~http_server()` 不等待停止完成，与文档承诺相反【高】

**证据**：`include/httplib/server/server.hpp:89-93`

```cpp
/// @brief 销毁 HTTP 服务器。
/// 若服务器仍在运行，析构会请求停止并等待其完成。
~http_server();
```

但 `lib/server/server_impl.cpp:484`：

```cpp
http_server::~http_server() { stop(); }   // 返回的 std::future 被丢弃，成员立即析构
```

**为什么不合理**：`stop()` 的 future（`server_impl.cpp:103-114`）被丢弃；一旦 io_context 同时
退出，stop 协程可能根本没机会跑完 —— 服务器「析构后」还在派协程访问已亡的 executor（UB）。

**建议**：析构内同步阻塞等待 future（带上限超时），或者把文档改成 fire-and-forget 并把
等待义务写进用户责任。二选一，不要两头都说。

### 2.2 proxy_client 读失败后不关闭连接（死代码）【高】

**证据**：`lib/client/proxy_client_impl.cpp:121-136`

```cpp
co_return co_await s->async_read_some(buffer, util::net_awaitable[ec]);
if (ec)                      // ← co_return 之后，永不可达
{
    co_await async_close();
}
```

**为什么不合理**：读失败后底层连接不会被 shutdown/close，只留半死 `http_stream`；
下次 `get_stream()` 报 `not_initialized`，但底层 fd/SSL 对象一直占着。同文件 `async_write`
（:139-158）与 `http_client::impl::async_read_some`（client_impl.h:244-278）是同构正确写法 ——
明显是复制改写遗漏。

**建议**：拆成 `auto n = co_await ...; if (ec) { co_await async_close(); } co_return n;`。

### 2.3 连接池归还经 `net::detached` 投递 —— ioContext 停转后连接永久悬挂【高】

**证据**：`lib/client/client_pool.cpp:199-211`

```cpp
void release(std::unique_ptr<http_client> conn)
{
    net::co_spawn(get_executor(),
        [this, self = shared_from_this(), conn = std::move(conn)]() mutable
            -> net::awaitable<void> { co_await async_release(std::move(conn)); ... },
        net::detached);
}
```

**为什么不合理**：po 后 executor 停转则 detached 协程永不调度；`conn` 滞留在挂起协程帧里，
不关闭、不回池、`total_active_` 不递减（`async_release` 是唯一 `-1` 处，:213-245），
产生幽灵连接。对照组 `http_client::impl::close()`（client_impl.cpp:433-444）用的
`use_future` 有兜底路径，此处没有。

**建议**：`release()` 先探测 executor 可用性，不可用直接析构 `conn`；或先同步修正计数再投递。

### 2.4 `write_impl_` 永不清理 → 连接永不回池；`create_lazy_request` 复用同一对象【高】

**证据**：

- 声明 `lib/client/client_impl.h:308`；全库 grep 该成员仅 4 处，**无 `.reset()`**
  （对照读方向 `read_impl_.reset()` 在 `response_impl.h:36-38` 有清理回调）。
- 复用判定：`lib/client/client_impl.cpp:110-115` —— `has_active_session()` 就是
  `!read_impl_.expired() || !write_impl_.expired()`。
- 创建：`lib/client/client_impl.cpp:409-420` —— 已持有时 `create_lazy_request()` 返回
  **同一个** `shared_ptr`。

**为什么不合理**：
1. 用户只要还持有一次 lazy_request 的引用，归还 `client_handle` 时该连接**一定被销毁**
   而不是回池（`client_pool.cpp:230-242`），池复用率被静默拉低；
2. 名义「创建」实际「返回当前活动写端」，写成两个变量名会误以为拿到两条独立请求通道。

**建议**：`lazy_request` 完成 `read_response*` 时（on_stored 同型回调）在 strand 上
`write_impl_.reset()`；`create_lazy_request` 语义文档化或改名（`borrow_lazy_request()`）。

### 2.5 默认构造的 `response` 半数成员方法是 UB【高】

**证据**：`lib/client/response.cpp`

- `response::response() = default;`（:17-23，`impl_` 为空 shared_ptr）
- 直接解引用无防护：`result()`(:25-29)、`result_int`(:31-35)、`operator[]`(:37-46)、
  `at`(:49-58)、`has`/`count`(:61-83)、`headers/base`(:85-108)、`content_length`(:110-119)、
  `as_string/as_json/type`(:121-149)、`read_string`(:163-191)
- 仅两处做了防护：`read_some_raw`(:202-208)、`is_body_done`(:244-248)，返回 `not_initialized`

**为什么不合理**：`error.hpp:28-29` 明确记录了 `not_initialized` 的语义「如空的 response」
—— 设计上打算全面判空，实际一半判一半不判，防护等于没有。

**建议**：所有成员统一走判空分支返回 `not_initialized`；或私有化默认构造，
要求通过 `result<response>` 获取实例。

### 2.6 per-route aspects 的所有权取决于实参值类别【中】

**证据**：`include/httplib/server/router.inl:117-124`

```cpp
if constexpr (sizeof...(Aspects) > 0)
{
    // std::tuple<std::decay_t<Aspects>...> aspects(std::move(asps)...);
    std::tuple<Aspects...> aspects(std::forward<Aspects>(asps)...);   // ← 存原始类型
}
```

- 传临时对象：tuple 持拷贝 —— 安全。
- 传 lvalue：tuple **存引用**，路由闭包长期持有调用方栈对象的地址（悬垂风险）。
- 对照全局 `router::use()`（router.inl:91-113）无一例外 `make_shared` 深拷贝。

**建议**：统一为 `router::use()` 的做法 —— 注册即拷贝进 shared_ptr；或文档明示
「aspect 必须比服务器活得久」并加 static_assert 栈地址断言。

### 2.7 `const request/response` 的 `base()` 返回可写借用视图【中】

**证据**：`lib/server/request.cpp:47-52`、`lib/client/response.cpp:86-90`、
`lib/client/request.cpp:177-182`

```cpp
httplib::headers request::base() const
{
    // unique_ptr 的 const 不传递到被指对象…
    return httplib::headers::impl::borrow(impl_->get());
}
```

**为什么不合理**：`headers` 借用视图承诺「写入直接落到本响应上」（headers.hpp:67-70），所以
const 重载与非常量重载**同语义** —— 经过 `const request&` 传递的所有代码里的 const 都是装饰，
多线程下极易造成数据竞争。

**建议**：const 版返回只读视图类型（`headers_view`），可写收口到非 const 重载。

### 2.8 sse/ndjson writer 持有 `stream_writer*` 裸指针 → `response::impl&`【高】

**证据**：

- `lib/server/sse_writer_impl.hpp:13,136` 与 `lib/server/ndjson_writer_impl.hpp:14,53`：
  `server::stream_writer* cw_;`
- `lib/server/stream_writer_impl.hpp:16,72`：`response::impl& resp_;`
- `lib/server/response.cpp:179-199`：`create_stream_writer()` 返回**裸指针**、
  `create_sse_writer()` 返回 **unique_ptr** —— 同类工厂两种所有权语义。

**为什么不合理**：SSE 常见模式是 handler 里 spawn 后台任务延后发事件；一旦 writer
（用户看似 `unique_ptr` 拥有）被带出 handler，`cw_` 悬垂到已析构的 response：三层
裸引用链没有任何一层表达所有权，约束既不编码也不写进接口注释。

**建议**：sse/ndjson writer 持 `shared_ptr<stream_writer>`，stream_writer 持
weak/shared `response::impl`；或文档写死「writer 生命周期 ≤ response」并加调试断言。

### 2.9 request/response 反向强持有 `http_task`，使用窗口无约束【中】

**证据**：`lib/server/request_impl.hpp:194-198`、`lib/server/response_impl.hpp:209-210` ——
request/response 持有 `shared_ptr<http_task>`；而 `http_task::read_header/read_some`
（session.hpp:96-124）与 keep-alive 主循环（session.cpp:255-411）共用同一 `buffer_` /
`raw_parser_`。

**为什么不合理**：所有权图 `session → task_ ← request/response` 是回环借用关系。handler
只要把 request / stream_reader 挪进延迟/detached 协程（lazy 读设计明确鼓励），就绕开
「单请求单窗口」隐含前提：本请求读协程与下一条请求的 header 解析在同一 buffer 上竞争，
框架没有断言拦截。HTTP/1.0 + `http_proxy_task`（session.cpp:385,500-551）生命周期更长。

**建议**：至少在 THREAD_MODEL / 头文件注明「request/response 须在 handler 内用完」；
更好：body_reader 用尽后置 invalid 标志，复用前断言。

### 2.10 `set_reverse_proxy`/`set_ws_forward` 的 location 惯例陷阱【中】

**证据**：`lib/server/server_impl.cpp:408-426`（location 原样传 router）、`:455`（ws 同）；
匹配语义见 `lib/server/proxy_util.hpp:17-26`（约定输入应是 `"prefix/*"`）；
全部测试都用 `/api/*`（tests/proxy_test.cpp:59 等）掩盖了问题。

**为什么不合理**：文档写「将 location 前缀下的请求反向代理」（server.hpp:303-305），但用户写
`set_reverse_proxy("/api", ...)` 时实际只匹配精确路径，`GET /api/foo` 静默 404，零提示。

**建议**：注册时校验 location：不含 `/*` 或以 `/` 结尾时自动补 `/*` 或
`throw std::invalid_argument`。

### 2.11 路由匹配语义缺陷（route confusion / 优先级丢失 / 回溯）【高+中】

三个相关子问题：

**a) 先 显式 url_decode 再分段**：`lib/server/request_impl.hpp:42-51`、`:58-66`；
路由树在解码后路径上匹配（`router_impl.cpp:334`），而注册 key 原样（:128）。
`/api%2Fsecret` 解码成 `/api/secret` 后可命中静态路由；mount point 用 wildcard
参数防穿越（mount_point_entry.cpp:147-161），客户端可以用 `%2F` 拼出预期外段数。
RFC 3986 里 `%2F` 与 `/` 不同 —— 常规做法是逐段解码而非整条。

**b) 同类节点注册优先级丢失 + 无界回溯 + 404 双遍历**：`lib/server/router_impl.cpp:374-458`
`match_nodes` 每层按 static→regex→param→wildcard 固定类序尝试，但 param/regex 无序
vector（:180-260 插入即合并），「先注册的 param 路由优先」契约（Express/cpp-httplib 通行）
不存在；一类失配回溯重展其他类（:402-435），深混合树失配时可指数级重放；未命中还要
整树二次遍历收集 allows（:351-368）。

**c) 尾斜杠不对称与段 trim**：`lib/server/router_impl.cpp:108-118` ——
`util::split`（lib/util/misc.cpp:38-41）对每段 trim + 去头尾分隔符；注册 `/x/` →
`["x",""]`，请求 `/x` → `["x"]`，互不 404 且文档未写该约定；含空格路径 `/ a /` 等价
`/a/` 意外命中。

**建议**：a) 逐段解码（`%2F` 保留为段内容）；b) param 子按注册序合并 + 距离优先，
allows 收集并入匹配过程；c) 注册时把尾斜杠归一（或 301 重定向），split 不要 trim 路径段。

### 2.12 `upstream_group` 健康机制是死代码；on_acquired/on_released 按字符串配对【中】

**证据**：

- `lib/server/upstream_group.cpp:140-156` 提供 `mark_unhealthy`/`mark_healthy`；
  全仓库调用点只有 tests/ —— 反代转发失败（`reverse_proxy_impl.cpp:286-324` 仅回 502 类
  文案）不标脏后端，rr/weighted/least_conn 会持续把 1/n 流量派给坏节点。
- `lib/server/upstream_group.cpp:61-78`、`reverse_proxy_impl.h:71-94` ——
  acquire/release 以 url 字符串精确配对；自定义 provider（公共接口唯一扩展点）返回
  大小写/尾斜杠/带 query 的变体时静默 miss → active 计数永久偏低 → least_conn 永远选它。

**建议**：失败路径回调 provider 钩子（`on_probe_failed` 类）并自动标脏 + 半开恢复；
lease 改为持不透明 ticket 而非再字符串匹配，miss 时至少打 debug 日志。

---

## 三、错误模型不统一（最大面积的设计债）

这是跨模块的共性设计问题，作为整体列出（每条均有代码证据）：

| # | 场景 | 现状（证据） |
|---|---|---|
| 3.1 | URL 解析失败 | client 构造抛 `std::invalid_argument`（`lib/client/client_impl.cpp:475-486`）；pool 返回 `error::invalid_url`（`client_pool.cpp:841-847`，注释还写明「统一映射」，client 没遵循） |
| 3.2 | 取 request body（服务端） | `as_*` 抛 `bad_variant_access`（request.cpp:155-177）/ `read_*` 抛 system_error（:185-233）/ `read_body()` 返回 ec / 内部 `take_materialized` 特意用 result（body_reader.hpp:241-251）—— 同一个概念四条路径四种表达 |
| 3.3 | `as_json()` 返回 `const&` 的稳定性 | body_state 可被 `read_body()` 重物化或 `read_string()` take 走（`take` 是移出语义，body_reader.hpp:250），await 点挂起期间引用即失效，无 API 保证 |
| 3.4 | `close()` 返回类型 | `client.hpp:138` `future<void>`（吞错） / `ws_client.hpp:179-187` `future<ec>` / `is_alive()` `future<bool>` |
| 3.5 | db pool 生命周期错误 | 借 generic `errc::operation_canceled`/`connection_aborted`（`connection_pool_impl.cpp:24-25,423`），与 `db::error` 枚举（error.hpp:15-32）两套 category；调用方要先判 category 再分流 |
| 3.6 | proxy CONNECT 拒绝 | Beast 原生 `http::error::bad_status` 直接漏出公共 API（`proxy_client_impl.cpp:103-108`） |
| 3.7 | downloader 重试策略 | `too_many_redirects`（结论性永久错误）被当可重试错误退避重试满 `max_retries`（`downloader_impl.cpp:505-514`）；预算耗尽兜底码是语义错位的 `errc::timed_out`（:510-543,700） |
| 3.8 | 重定向预算耗尽 | `http_client` 拿回最后的 3xx 响应且 client_impl.cpp:280 兜底码**永不可达**；downloader 得到 `error::too_many_redirects`（:315-317）—— 两个实现行为分叉 |
| 3.9 | JWT claim getter | 构造号称不抛（jwt.hpp:120-123），`get_issuer()` 等缺 claim 时抛 `out_of_range`（旧 jwt.cpp:302-341） |
| 3.10 | 非 SSL 构建 JWT | 签名实现深处抛裸 `std::runtime_error`（jwt.cpp:173-210），不经模块 `error` 枚举 |

> **✅ 3.10 已随 jwt→jwt-cpp 重写消除（2026-10）**：jwt-cpp 恒依赖 OpenSSL，OpenSSL 与
> `jwt-cpp::jwt-cpp` 成为库的无条件依赖（`lib/CMakeLists.txt`），HS* 在任何构建配置下均可用，
> 非 SSL 抛异常路径整条删除；`httplib_test_jwt` 相应解除 `HTTPLIB_ENABLED_SSL` 门控。

**建议**：先定一条全库契约再逐点整理。推荐：

> **公共异步 API 一律 `error_code` 传播（必要时返回 `boost::system::result`）；
> 同步便捷层提供「抛异常薄包装」；用户可重入的快照查询用 optional/哨兵时必须加显式
> 结束标志（见 §5.5）。**

该决策一次落地后，3.1–3.10 都是机械改写。

---

## 四、性能热点

### 4.1 每条新连接新建 `ssl::context`【中】

**证据**：`lib/stream/http_stream.hpp:225-277`（`http_stream::create` 内），调用点
`client_impl.cpp:369-378`、`proxy_client_impl.cpp:54`、`ws_client_impl.cpp:45`。

SSL_CTX 初始化（算法表、密码套件、会话缓存）开销大且是会话复用/票据单元；
池累计新建的每条 HTTPS 连接都重付一次，无缓存路径。

**建议**：按 (verify 标志, CA 哈希) 缓存 `shared_ptr<ssl::context>`，
`create()` 只做 per-stream SNI + verify_callback。

### 4.2 middleware 全局互斥锁把关键路径串行化【中】

- `rate_limit`：`lib/server/middleware/rate_limit.cpp:28-29` —— 全部桶共用一把全局
  `std::mutex` + `string_map`；每个请求 before 阶段都要进这把锁 lookup+更新。
- `session` store：`session_mw.cpp:202`（全锁）、`:227-241`（sweep 持锁 O(n) 全表 erase）、
  `:252-258`（evict 线扫最旧）。容量逼近上限时**每个新 session 创建**触发一次持锁全表扫描，
  锁争用时间与存储量线性相关。

**建议**：两处都按 key hash 分片（N 片各一把锁/一个 strand）；session 增加按
last_access 排序的辅助索引，锁内 O(log n) 摘除；或容量满时拒新（fail-fast）。

### 4.3 `when_all` 用 `std::function` 载体 + 无 fail-fast + 左值重载掏空入参【中】

**证据**：`include/httplib/util/when_all.hpp`

- `:112,127,165-170` —— vector<function<awaitable()>>，move-only awaitable 还要再包一层
  `shared_ptr`：N 个操作 ≈ 3N 次堆分配。
- `:130-131,193-194` —— `wait_for_all()`：一路挂死整体永不返回；一路失败其余继续跑完。
- `:112` 左值引用重载内 `std::move(op)` —— 传 lvalue vector 却被搬空，违反最小惊讶。
- `:174,213` —— 公共头里的 **static** 自由函数（每个 TU 一份拷贝，膨胀目标文件）。

**建议**：接口改 `vector<F>&&` / 可变参数模板（:235-245 已有正确雏形）；
提供 `wait_for_one_error` 变体；static 去掉、inline 化。

### 4.4 `request_data` 锁 + typeid 字符串 key【中】

**证据**：`include/httplib/server/request_data.hpp:28-29,49,73,86`（每个操作
`lock_guard`）、`:100-112`（每次 `typeid(type).name()` 拼 key 字符串）。

request 生命周期内单 strand 独占（THREAD_MODEL 拓扑本身就是为此设计），该锁
把「谁在并发访问 request」这个本应暴露的问题藏了起来，还付出锁+字符串构造常数。

**建议**：文档化「仅可在会话 strand 上访问」并去锁；key 用编译期 hash 或 MW 注册表 index。

### 4.5 路由匹配与 404 双遍历

已在 §2.11.b 覆盖：有界回溯可统计上界约束，404 是最高频请求却吃补丁式 O(2×树)。
建议 allows 收集并入单遍匹配。

### 4.6 headers 每字段一次堆分配【低】

**证据**：`lib/headers.cpp:49-54`

```cpp
header::header(header const& other) : impl_(std::make_unique<impl>(*other.impl_)) {}
```

作者注释自己承认（:47-48）「拷贝一次即一次堆分配：遍历时务必用 auto const&」并把责任推给
调用侧。迭代应产生无所有权的 `header_view`，需要持久化时再升级 owned。

### 4.7 杂项

- `async_mutex` waiter 用 `deque<shared_ptr>` + cancel O(n) 摘除（`async_mutex_impl.hpp:512,402-406`）
  —— 建议 intrusive hook。
- `least_conn` 选择无原子性：并发请求同选最小后端的窗口（upstream_group.cpp:221-245）。
- 三种 base64 实现并存（auth.cpp:12-40 / jwt.cpp:100-124 / 其它）→ 收敛到 util。

---

## 五、静默配置陷阱（fail-open / fail-silent）

以下均为「写错了也不告诉你」的配置/运行行为，建议 fail-fast：

| # | 问题 | 证据 | 建议 |
|---|---|---|---|
| 5.1 | handler 抛异常时**整条 after 链被跳过**，无 error 钩子 | `router.inl:148-155`（`if (!eptr) ... do_after`）、`router_impl.cpp:161-171`（全局层同样） | 提供 `after(req, resp, error)` 形态，或异常路径保证 after 执行且可读错误 |
| 5.2 | 全局 `use()` 在路由注册**之后**对新路由不生效，无告警 | `router_impl.cpp:130`（wrap_global 在注册时固化捕获 `global_before_`/`global_after_`） | 改运行期查表；或 run() 校验 use 与注册的时间序 |
| 5.3 | 405/404 响应不经过任何全局中间件 | `session.cpp:305-313`（未命中直接 `set_error_content`）；限流/CORS/审计对「最常被攻击的 404」不生效 | 全局 before/after 由 session 在派发前显式调用 |
| 5.4 | accept 循环任何非 EMFILE 错误永久退环，无自愈无上报 | `server_impl.cpp:216-232`（其余错误 `break`） | ECONNABORTED/EINTR 类 continue；异常退出上报 ec |
| 5.5 | SSE/NDJSON 用「空载荷/空 value」当 EOF 哨兵，与合法数据同形 | `sse_reader_impl.hpp:31-36`（EOF := `sse_event{}`）、`ndjson_reader_impl.hpp:88-91`（EOF := null，而 RFC 7464 keep-alive 就是 null 行） | 返回 `result<std::optional<T>>` 或加 `is_eof` 字段 |
| 5.6 | `Location: //host/path` 协议相对重定向被当相对路径发给原主机 | `client_impl.cpp:216-251`、`http_header_util.hpp:246-261`（两处独立实现，均只判 `http://`/`https://` 前缀） | 抽出共用 `classify_location()` 处理 `//` 分支 |
| 5.7 | 重定向 drain 把 body 整体读进内存（OOM 面） | `client_impl.cpp:209-213`（`read_string()`）、`downloader_impl.cpp:304-311,557-563`（`read_body()`） | 提供 `response::drain()`（空 sink） |
| 5.8 | disk_cache 只用 64 位 FNV 哈希做 key，冲突即静默返回错误内容 | `disk_cache_impl.cpp:44-56,629-693,786-797`（put 冲突 `remove_all` 别的 entry；get 无 key 比对） | entry 落盘完整 key，命中后比对，不匹配按 miss 处理 |
| 5.9 | pool `stats()` future 式重载的 `string_view` 参数可能悬空 | `client_pool.cpp:890-902`（co_spawn 延迟执行，临时 string 已亡）；对照 `downloader_impl.cpp:75-84` 的正确写法已存在 | 参数先拷贝 std::string |
| 5.10 | `max_size == 0`（禁新路由）与 `max_total == 0`（不限）语义冲突未文档化 | `client_pool.cpp:374-377` vs `client_pool.hpp:19-25` | 文档写明或统一 0=不限 |
| 5.11 | ws/proxy `async_connect` 已连接时静默忽略新 target（ws 侧还用 ping 探测、未文档化） | `ws_client_impl.cpp:33-41`、`proxy_client_impl.cpp:41-45` | 显式检查已连接状态 + 文档化 or 返回专用错误 | 
| 5.12 | before 返回 false 且未写响应时落回「默认 404 页」（响应默认状态是 not_found） | `response_impl.hpp:48`、`session.cpp:312` | 调试日志提示「before 短路但响应未写」 |
| 5.13 | url_decode 静默吞非法编码、url_encode 空格输出 `+`（form 约定用于 path 场景） | `url.cpp:39-55`、`:65-89` | 加 strict 变体（ec/result）；拆 `form_urlencode` 与 RFC3986 编码 |
| 5.14 | 正则段缺 `:` 名时 param_name 退化成整个 pattern 堆进 params | `router_impl.cpp:229-247` | 注册期报错 |
| 5.15 | `on_released` 字符串配对 miss 时静默 | 见 §2.12 | 见 §2.12 |

---

## 六、API 一致性杂项

| # | 问题 | 证据 |
|---|---|---|
| 6.1 | ws_client 与 server 端回调命名相同形态完全不同：server 带 `websocket_conn::weak_ptr`，client 的 close 回调**无参**（拿不到死因 ec）；client 读循环把 ec 丢弃 | `ws_client.hpp:322-324` vs `websocket_conn.hpp:37-42`；`ws_client_impl.cpp:368-393` |
| 6.2 | server `close(reason)` 带 reason，client 恒 `"normal"` | `websocket_conn.hpp:66-76` vs `ws_client.hpp:178-187`、`ws_client_impl.cpp:323` |
| 6.3 | 三个 writer 的 ec/throw 双重载是机械复制，而 request read 族又是另一套单身形态 | `stream_writer.hpp:24-33`、`sse_writer.hpp:15-35`、`ndjson_writer.hpp:15-19` vs `request.cpp:236-267` |
| 6.4 | `stream_writer::mode::relay` 是反代内部语义却放公共 API，普通用户写 relay 得理解上游分帧 | `stream_writer.hpp:16-22`、`stream_writer_impl.hpp:37-44` |
| 6.5 | `stream_writer` 空 statements: `create_stream_writer` 裸指针 vs `create_sse_writer` unique_ptr（同 §2.8） | `response.cpp:179-199` |
| 6.6 | websocket open handler 抛异常时不关连接、close_handler 不触发；正常路径 close 必回调 —— 生命周期不对称 | `websocket_conn_impl.cpp:238-246`（catch 后 `co_return`，跳过 :274-281） |
| 6.7 | websocket 读循环无 idle/timeout 定时器（HTTP 路径全程 `expires_after`，WS 没有），半开连接永久占会话表 | `websocket_conn_impl.cpp:248-262` |
| 6.8 | downloader `download_single` 对永久错误也退避重试；预算耗尽兜底码 `errc::timed_out` 语义错位 | `downloader_impl.cpp:505-514,700`（详见 §3.7） |
| 6.9 | `fnv1a_64`/`fnv1a_hex` 两处重复实现；base64 三处重复 | `disk_cache_impl.cpp:44-56` vs `http_header_util.hpp:18-28`；auth.cpp:12-40 |
| 6.10 | ODBC 手工 `#define` SQL Server 按私结构并按推测布局用 `SQL_C_BINARY` 解引用 —— 驱动内布局变更是静默错位 | `odbc_backend.cpp:13-46,707,735` |
| 6.11 | db 三后端 bind/read 样板同构三写（sqlite 文本 4 分支重复、odbc CHAR/BINARY probe-read 双拼、mysql if/else+static_assert 兜底）—— 新类型需三处同步 | `sqlite_backend.cpp:97-124`、`odbc_backend.cpp:409-520`、`mysql_backend.cpp:77` |
| 6.12 | `middleware::data.hpp::store` 要调用方先包 `std::any` 再 `any_cast` 回来，错误时才 bad_any_cast | `include/httplib/server/middleware/data.hpp:31-36` |
| 6.13 | `query_params()`(URL) 与 `as_query_params()`(body) 同名不同物，极易取错 | `server/request.hpp:59` vs `:75` |
| 6.14 | response 缺 `keep_alive()` 读口；content setter 各带冗余 status 参数 | `server/response.hpp:48-69` |
| 6.15 | `verifier::allow_algorithm` 单槽覆盖语义，与「allow 白名单」名义相悖，不支持算法轮换 | `jwt.hpp:267`、旧 `jwt.cpp:513-518` |
| 6.16 | `websocket_stream` 缺硬关闭原语（teardown/abort），ws_client 只能拿 `socket()` 裸 shut/close 绕过抽象 | `ws_client_impl.cpp:468-480`；`http_stream.hpp:125-142` 已有现成模式可搬 |
| 6.17 | `http_stream::is_peer_alive` 永久改写 socket non_blocking 且不恢复；`close()` shutdown 的错误被 close 覆盖 | `http_stream.hpp:77-94`、`:137-142` |
| 6.18 | `url_decode` 无错误通道 + `to_url()` 对 IPv6 用裸 `find(':')` 判断（未检查已带括号 → `[[::1]]`） | `url.cpp:124-129`、`:186-199` |
| 6.19 | `parse_url` 丢弃 userinfo，URL 身份信息静默消失（host 白名单/缓存键场景有歧义风险） | `url.cpp:152-160` |
| 6.20 | 枚举名表三处人工镜像（enums.hpp / enum_conv.hpp 460+ 行 / test 名单），违约静默降级 unknown | `lib/enum_conv.hpp:39-497` 及头部注释自述 |
| 6.21 | 可选功能宏曾以 `target_compile_definitions` PUBLIC 注入下游：开关选项同时改变符号有无、ABI 与下游构建要求三件事，下游必须自己记得加同名 `-D` 才能编译；漏带即头文件与库 ABI 静默分叉 | 曾见 `lib/CMakeLists.txt` 各 feature 分支 |

> **✅ 已修正（2026-10）**：统一为「**常在接口 + 运行期失效**」：
> 公共头（含 `jwt*` / `middleware/jwt_auth.hpp`）
> 恒定声明 API、零宏；选项经根 `CMakeLists.txt` 汇总后以 `PRIVATE` 编译定义只
> 注入库自身（测试/示例镜像同一组），宏不进任何导出接口。未启用能力的运行期收口：SSL 抛
> `protocol_not_supported`；JWT 在各自实现文件内以 `#ifdef/#else` 双段定义，
> 失效桩段兜底全部公共入口（非 noexcept 抛错 / noexcept `abort`）；DB 基座常编、
> 注册表返回 `unknown backend` 错误码；COMPRESS 无公共 API。

---

## 七、勘误

复核过程中修正了初步分析的 1 条误报：

- **「set_ws_handler 接收 aspects 并静默丢弃」→ 降级为 API 整洁问题。**
  实际代码（`include/httplib/server/router.inl:216-227`）声明了尾部参数包
  `typename... Aspects` 但函数签名未接收任何 aspect 形参；传入额外实参会**在编译期
  失败**（推导无绑定位置），并非静默丢失。保留此条是因为它说明 WS 管线曾设计过
  aspects —— 应删除死参数包或真正实现 WS aspect 支持。

另：§2.1 的 `stop()` future 丢弃、§2.2 的死代码、§1.1 的 fail-open 已逐一对照源码确认为实。

---

## 八、优先级总表

### P0（安全，立即修）

| # | 条目 | 一句话修复 |
|---|---|---|
| §1.1 ✅ | JWT verifier 默认不验签 | `if (!alg_) ec = not_initialized; return;` |
| §1.2 ✅ | `sign()` 丢 header claims | `header_.emplace(algorithm, ...)` 而非整体赋值 |
| §1.3 ✅ | WS 升级 404 不回响应 | 未命中走 HTTP 404 |
| §1.4 ✅ | 流式响应失败不标脏连接（协议 desync） | header 上线闩锁 + 上线后拒绝改写 |
| §1.5 ✅ | CORS 多 origin 静默失效 | 回显请求 Origin + 白名单 + Vary |
| §1.6 ✅ | session id MT19937 | boost UUID 随机生成器（id 整体不透明化） |

### P1（正确性）

| # | 条目 |
|---|---|
| §2.1 | 析构不等待 stop（文档与实现矛盾） |
| §2.2 | proxy_client 读失败死代码 |
| §2.3 | pool 归还 detached spawn |
| §2.4 | `write_impl_` 永不清理 / create_lazy_request 复用 |
| §2.5 | 默认构造 response 半数 UB |
| §2.6 | per-route aspects 所有权 |
| §2.10 | reverse_proxy location `/*` 校验 |
| §2.11 | 路由匹配语义三连（%2F / 优先级 / 尾斜杠） |
| §2.12 | 上游健康机制死代码 + ticket 配对 |

### P2（契约整理）

| 类别 | 内容 |
|---|---|
| §三 全部 | 全库统一错误模型（先定契约再机械改写） |
| §2.7 | const 收口为只读视图 |
| §2.8 / §6.5 | writer 所有权统一 |
| §5.5 | EOF 哨兵歧义 |
| §2.9 | request/response 使用窗口文档化 + 失效断言 |

### P3（性能 / 基建）

| # | 条目 |
|---|---|
| §4.1 | ssl::context 缓存 |
| §4.2 | rate_limit / session 分片 |
| §4.3 | when_all 去 function 化 + fail-fast |
| §4.4 | request_data 去锁去 typeid |
| §4.5 | 路由匹配收敛单遍 |
| §4.6 | header_view 迭代 |
| §六 全部 | API 一致性顺手收敛 |

---

## 附：建议整改顺序总结

技术上最值得先做的三件事：

1. **在 THREAD_MODEL.md 同级新增 ERROR_MODEL.md，先给出全库错误模型契约**
   （§三）—— 一次决策解掉十几个分散问题，收益最大；
2. **修 §1.1 / §1.2 两处 JWT fail-open**—— 改动量小、无 API 破坏、价值最高；
3. **给流式响应补 `mark_broken()` / keep-alive 标脏（§1.4）** —— 这是唯一会跨连接
   产生协议错乱的问题。
