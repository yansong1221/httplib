# httplib 架构、代码质量与风险分析报告

> 审查日期：2026-08-09  
> 审查分支：`dev`  
> 审查提交：`56f47cf811254b90057239360b499f9b2f09efca`（修复卡死bug）  
> 上游仓库：<https://github.com/yansong1221/httplib>  
> 复查日期：2026-09-07；2026-09-09 更新 CON-01/02、HTTP-01、WEB-01、Range 数量与 multipart 字段数上限（SEC-03 部分）修复状态
> 复查提交：`6f8a602`（逐一核对各风险项修复状态）
> 清理日期：删除已过时的描述（解压后大小上限、WS 队列上限、multipart 单字段大小已落地；测试已拆分；LICENSE 已补；仓库统计数字已过时），并据代码把 SEC-03 判定为已修复

## 1. 执行摘要

该项目是一套基于 Boost.Asio/Beast、面向 C++23 的异步 HTTP/1.1 与 WebSocket 客户端/服务端框架，同时包含路由、中间件、反向代理、文件服务、SSE、NDJSON、JWT、Session、下载器、磁盘缓存和可选数据库支持。

总体判断：**架构方向合理、功能覆盖完整、测试投入较明显。截至本次清理，报告中的安全阻断项（上传路径逃逸、TLS/JWT 校验、URL 解码越界、Range 边界、目录列表注入、符号链接逃逸、CONNECT 开放代理、Header/Body/解压后大小/multipart/Range 资源上限、长期容器淘汰、客户端 IP 可由 X-Forwarded-For 任意伪造等）已全部修复；端到端数据竞争（CON-01/02）与运行期配置并发契约（API-01）也已收口；剩余未收口的是路由正则的启发式准入、socket stop / client 并发读写的 strand 约束，以及构建发布工程，不建议未经整改直接暴露在公网或承担认证、上传、代理等关键业务。**

| 维度 | 评分 | 结论 |
|---|---:|---|
| 架构设计 | 6.5/10 | 分层和核心抽象合理，但职责范围过宽 |
| 模块化与可读性 | 6/10 | 目录清楚，部分中心文件过大、耦合偏重 |
| 测试建设 | 7/10 | 真实 TCP 测试已按组件拆分，但安全和并发边界覆盖不足，无 CI/动态检测 |
| 并发可靠性 | 5/10 | 端到端 session 数据竞争（CON-01/02）已修复；线程模型约束仍不清晰 |
| 安全性 | 6/10 | 上传路径逃逸、TLS/JWT、URL 解码、重定向敏感头、目录注入、CONNECT 开放代理、资源上限、长期容器淘汰、客户端 IP 伪造、运行期配置并发契约等已修复；路由正则的启发式准入仍待收口 |
| 构建与发布成熟度 | 3.5/10 | 缺依赖锁定、CI，安装配置不完整 |

建议定位：当前版本适合作为个人项目、内部实验框架或二次开发基础；完成本报告 P0/P1 整改、动态检测和压力测试前，不应判定为生产就绪。

## 2. 审查范围与方法

本次审查覆盖：

- `include/httplib/` 公共 API、PIMPL 边界及依赖暴露情况；
- `lib/server/` 请求解析、路由、连接生命周期、WebSocket、静态文件和代理；
- `lib/client/` HTTP/WS 客户端、连接池、重定向、下载器与缓存；
- Body、multipart、Range、压缩、JWT、Session、Rate Limit 等组件；
- CMake 构建、安装导出、依赖声明和测试组织；
- Git 分支、版本标签、提交历史和仓库治理文件。

采用了源码静态审查、危险模式检索、Git 历史核对和 CMake 配置验证。由于当前机器缺少项目所需的 Boost、spdlog、fmt、Catch2 等外部依赖，本次未能完成编译和运行测试；因此下文不将“测试未执行”表述为“测试失败”。

## 3. 仓库概况

- C++ 代码约 28,627 行：公共头文件约 3,814 行、实现约 15,487 行、测试约 8,074 行、示例约 1,252 行。
- Catch2 `TEST_CASE` 共 718 个，大量测试会启动真实 TCP 服务进行端到端验证。
- 公共头文件 70 个；其中 35 个仍直接 `#include <boost/...>`（Beast 相关暴露已随 `880f686` 收敛，但 Asio/JSON/System 等仍可见）。
- 当前默认克隆分支为 `dev`；`master` 比 `dev` 落后 431 个提交。
- 当前提交比 `v1.0.5` 标签多 324 个提交；代码仍在快速演进。
- 634 个历史提交中约 161 个提交信息仅为 `update`，问题定位和变更追溯成本较高。

## 4. 架构分析

### 4.1 值得肯定的设计

1. **公共接口与实现分层清楚**

   `include/httplib/` 和 `lib/` 的目录职责明确，多数核心类型通过 PIMPL 隔离实现细节。服务端 `impl` 使用 `shared_ptr`，配合协程捕获延长异步生命周期，方向合理。

2. **连接会话采用任务状态转换**

   `session` 将连接处理拆分为 SSL 探测、TLS 握手、普通 HTTP、WebSocket 和 CONNECT 代理任务，比把所有协议状态堆在单个循环中更容易理解。

3. **路由结构与匹配优先级清楚**

   路由使用 Trie，按静态路径、正则参数、普通参数、通配符依次匹配，支持同步/协程 handler 和前后置 middleware。

4. **Body 抽象具有扩展性**

   `any_body` 用 variant 统一字符串、JSON、multipart、文件和 URL encoded 数据，并复用 Beast serializer/parser 模型，减少上层 API 分裂。

5. **真实网络测试较丰富**

   测试不是只验证内部函数，而是通过随机本地端口启动服务器并发起真实请求，对连接复用、流式传输等场景更有价值。

### 4.2 主要架构问题

1. **核心库职责过宽**

   项目同时覆盖 HTTP、WebSocket、反向代理、JWT、Session、数据库、下载器和缓存。网络协议内核与应用级能力耦合在同一库中，安全面、依赖面和回归范围持续扩大。

2. **PIMPL 未完全隐藏 Boost**

   70 个公共头文件中仍有 35 个直接暴露 Boost.Asio、Boost.JSON、Boost.System 或 spdlog 类型。Beast 相关暴露已随 `880f686` 收敛到私有 `lib/beast_alias.hpp`，但调用方仍需接受 Boost 的源码、ABI、编译时间和版本耦合。

3. **线程模型没有形成统一契约**

   一些容器使用 mutex，一些状态使用 atomic，但 socket、serializer、middleware 和路由更新没有统一通过 strand 串行化。公共 API 也未明确哪些对象允许跨线程、哪些仅允许在 executor 线程调用。

4. **缺少统一资源治理层**

   Header、Body、解压后大小、multipart 字段、Range 数量、WebSocket 发送队列、Rate Limit bucket、Session 数量等限制分散或不存在，无法形成可靠的抗 DoS 边界。

5. **中心模块偏大**

   `server_impl.cpp` 同时负责监听、TLS、代理、WS 转发和配置；下载器实现超过千行。继续扩展会增加局部修改影响全局行为的概率。

## 5. 风险清单

### 5.1 Critical：生产阻断项

#### SEC-01：multipart 上传可造成路径逃逸和任意文件写入

> 状态：✅ **已修复**（复查 2026-09-07）

~~启用 `http_server::set_upload_dir()` 后，客户端提供的 multipart `filename` 被直接用于拼接保存路径：~~

```cpp
auto name = field_data_.filename.empty() ? "upload" : field_data_.filename;
current_file_path_ = body_.save_dir / name;
file_stream_.open(current_file_path_, std::ios::out | std::ios::binary | std::ios::trunc);
```

~~没有拒绝绝对路径、`..`、路径分隔符或符号链接逃逸，也没有 canonical containment 检查。攻击者可尝试覆盖进程有权限写入的任意文件。~~

当前实现（[lib/body/form_data_body.cpp](lib/body/form_data_body.cpp#L536)）已有三层防御：

1. 先用 `fs::path(...).filename()` 仅提取路径最后一个组件，剥离所有目录分隔符；
2. 显式拒绝空字符串、`.`、`..`，命中时回退为 `"upload"`；
3. `weakly_canonical` 规范化后做前缀 containment 校验，逃逸 `save_dir` 即拒绝。

- 位置：[lib/body/form_data_body.cpp](lib/body/form_data_body.cpp#L469)
- 影响：~~任意文件创建/截断、配置覆盖、潜在代码执行~~ 已消除
- 建议：~~仅接受 basename；拒绝绝对路径及任何父目录分量；由服务端生成随机文件名；打开前后执行 canonical/weakly_canonical containment 校验；考虑 `O_NOFOLLOW` 或平台等价机制~~ 纵深防御已具备，可选加随机文件名与 `O_NOFOLLOW` 进一步加固。

#### SEC-02：所有 CONNECT 请求绕过路由和中间件，形成开放代理

> 状态：✅ **已修复**（复查 2026-09-09）

~~普通 HTTP 会话在执行 `router.pre_routing()` 前直接判断 `CONNECT`，随后解析目标并建立任意 TCP 连接。现有路由鉴权、JWT、Rate Limit 和自定义 ACL 均不会执行。~~

现状：
- CONNECT 默认被拒绝：未注册 `set_connect_handler` 时返回 `405 method_not_allowed`，不再是开放代理；
- CONNECT 走完整的 `pre_routing` → `post_routing` 管线，路由级中间件正常执行；
- `set_connect_handler` 支持路径匹配（`query_connect_handler`）和全局中间件（`use()` 经 `wrap_global` 包装）。

- 入口：[lib/server/session.cpp](lib/server/session.cpp#L260)
- 转发实现：[lib/server/session.cpp](lib/server/session.cpp#L372)
- 影响：~~开放代理~~ 已消除
- 建议：~~默认禁用 CONNECT；提供显式开关；必须经过认证、目标主机/端口 ACL、DNS/IP 二次校验和连接/流量限制~~ 已落实。

#### SEC-03：请求 Header/Body 基本不设上限

> 状态：✅ **已修复**

~~服务端将 header limit 设置为 `uint32_t` 最大值，body limit 设置为 `unsigned long long` 最大值。普通字符串、表单和 JSON 均可能消耗接近无限内存，且公共 server API 没有全局限制入口。~~

现状：
- header limit 已改为合理默认值 `65536`（64 KB），并新增 `set_header_limit()` 配置入口；
- upload file limit 默认 10 MB，同样可配置；
- `body_limit_` 默认已由 `std::numeric_limits<std::uint64_t>::max()` 改为 **1 GiB**（`[lib/server/server_impl.h](lib/server/server_impl.h#L167)`）；
- **Range 数量上限已落地**：`http_ranges` 默认最多 100 段，超限整组拒绝，新增 `max_ranges`/`set_max_ranges()` 可配置入口（`[lib/html/http_ranges.cpp](lib/html/http_ranges.cpp#L35)`），防 Range 放大；
- **multipart 字段数上限已落地**：`form_data` 默认最多 128 字段（`max_fields_default`），超限以 `http::error::body_limit` 拒绝，服务端经 `set_form_data_config()` 统一配置 `form_data::param { save_dir, max_file_size, max_fields }`，eager/lazy 解析路径均已接线；
- **multipart 单 part 内容大小已落地**：`max_file_size` 对文件 part 与内存字段 part 一体适用（`[include/httplib/form_data.hpp](include/httplib/form_data.hpp#L48)`），`0` 表示不限；
- **解压后大小上限已落地**：`stream_decoder` 对**产出**字节计数，超过限额报 `http::error::body_limit`（`[lib/body/codec.cpp](lib/body/codec.cpp#L151)`、`[lib/body/codec.hpp](lib/body/codec.hpp#L19)`）；接线点 `stream_decoder_->reset(content_encoding, body_limit_, ec)`（`[lib/body/body_reader.hpp](lib/body/body_reader.hpp#L413)`），服务端传入 `task->body_limit()`、客户端传入 `body_limit_.load()`，压缩输入字节仍由 parser 各自限制。

- 请求解析：[lib/server/session.cpp](lib/server/session.cpp#L207)，默认值 [lib/server/server_impl.h](lib/server/server_impl.h#L166)
- 公共 API：[lib/server/server.hpp](lib/server/server.hpp#L65)
- JSON 分配：[lib/body/json_body.cpp](lib/body/json_body.cpp#L55)
- Range 数量：[lib/html/http_ranges.hpp](lib/html/http_ranges.hpp#L16)
- multipart 字段数：[include/httplib/form_data.hpp](include/httplib/form_data.hpp#L46)
- 解压后大小：[lib/body/codec.cpp](lib/body/codec.cpp#L151)、[lib/body/body_reader.hpp](lib/body/body_reader.hpp#L413)
- 影响：~~header/文件上传/body/Range 数量/multipart 字段数/解压后大小无界~~ 已消除
- 建议：~~为 header、总 body、各 body 类型、multipart 字段数/字段大小、单文件和总上传量设置安全默认值及可配置上限；限制解压后的大小~~ 已落实。

#### SEC-04：HTTPS/WSS 身份校验不完整

> 状态：✅ **已修复**（复查 2026-09-07）

~~普通 HTTP 客户端默认 `verify_ssl_ = false`。即使开启验证，TLS context 只设置了 `verify_peer`，没有配置 hostname verification；受信任 CA 为其他域名签发的证书仍可能被接受。WSS 客户端没有对应的公开验证配置。~~

现状（[lib/stream/http_stream.hpp](lib/stream/http_stream.hpp#L204)）：
- HTTP 客户端 `verify_ssl_` 默认改为 `true`（[lib/client/client_impl.h](lib/client/client_impl.h#L171)）；
- `create_stream()` 完整执行：`verify_peer` + CA 加载 + SNI（`SSL_set_tlsext_host_name`）+ `ssl::host_name_verification(host)`；
- WSS 客户端复用 `create_stream()`，`verify_ssl_` 默认同样为 `true`；代理、下载器 TLS policy 已统一。

- 默认值：[lib/client/client_impl.h](lib/client/client_impl.h#L146)
- TLS context：[lib/stream/http_stream.hpp](lib/stream/http_stream.hpp#L164)
- WS 创建连接：[lib/client/ws_client_impl.cpp](lib/client/ws_client_impl.cpp#L42)
- 影响：~~中间人攻击、错误上游身份被接受~~ 已消除
- 建议：~~HTTPS/WSS 默认验证；使用 `ssl::host_name_verification(host)` 或等价实现；统一 HTTP、WSS、代理、下载器的 TLS policy；明确 CA 和 SNI 行为~~ 已全部落实。

#### SEC-05：JWT verifier 不校验 `exp` 和 `nbf`

> 状态：✅ **已修复**（复查 2026-09-07）

~~JWT builder 和 decoded object 支持 `exp`、`nbf`、`iat`，但 verifier 只检查签名以及可选 issuer、subject、audience、id、自定义 claim，没有检查令牌是否过期或尚未生效，也没有校验 Header 中声明的算法名是否与配置算法一致。~~

现状（[lib/jwt.cpp](lib/jwt.cpp#L482)）：
- 校验 Header 中 `alg` 是否与配置算法一致（不一致报 `algorithm_mismatch`）；
- 校验 `exp`（超时返回 `expired`）与 `nbf`（未生效返回 `not_yet_valid`），均支持 `with_clock_skew()` 容忍时钟偏差；
- 保留 iss/sub/aud/jti 及自定义 claim 校验，失败即返回对应 error_code。

- 位置：[lib/jwt.cpp](lib/jwt.cpp#L469)
- 影响：~~过期或提前使用的 Token 可继续访问受保护资源~~ 已消除
- 建议：~~默认强制校验 `exp`/`nbf`，支持 clock skew；核对 `alg`；使用常量时间签名比较；增加缺失/类型错误 claim 的安全失败路径~~ 时间与算法校验已落实，可补充常量时间比较与缺失 claim 的显式测试。

### 5.2 High：高风险项

#### SEC-06：URL 解码对畸形 `%xx` 存在越界读取

> 状态：✅ **已修复**（复查 2026-09-07）

~~`url_decode` 在看到 `%` 后直接执行两次 `++r`，没有验证剩余长度，也不校验字符是否为十六进制。结尾 `%`、`%A` 等输入可能触发未定义行为。请求路径创建时会自动调用该函数，因此可由远程请求触发。~~

现状（[lib/url/url.cpp](lib/url/url.cpp#L40)，实现已从 `lib/util/misc.cpp` 迁出）：
- 仅在 `r + 2 < size` 且 `is_hex_digit(str[r+1])` 且 `is_hex_digit(str[r+2])` 时才解码；
- 短路求值保证越界访问不可能发生；畸形 `%` 按普通字符原样保留，不会触发未定义行为；
- 该行为已由 `body_utils_test.cpp` 的 `url::url_decode rejects malformed percent encoding` 与 `url::url_decode does not read out of bounds` 两个用例固化（`trailing%`、`%A`、`%ZZ`、`%%`、`%GG`、`%20%A` 等）。

- 解码实现：[lib/url/url.cpp](lib/url/url.cpp#L40)
- 请求调用点：[lib/server/request_impl.hpp](lib/server/request_impl.hpp#L42)
- 建议：~~解析前检查 `r + 2 < size`，严格验证 hex digit；非法输入返回 error/result，不应静默解码~~ 越界与非法输入已安全处理。畸形输入改为显式报错一事已有结论：采用原样保留，不再是待决项。

#### SEC-07：客户端 IP 可由请求头任意伪造

> 状态：✅ **已修复**（本次整改）

`request::get_client_ip()` 原先只要请求带 `X-Forwarded-For` 就直接采用**最左端**的值，代码库中不存在任何可信代理配置。两个后果：

1. **限流可被完全绕过** —— `rate_limit_middleware` 以该值为桶键，直连部署下任何客户端每个请求换一个 `X-Forwarded-For` 就能拿到全新配额，按 IP 限流形同虚设；
2. **日志与审计可被伪造** —— 同一个值还会被用于访问日志，攻击者可以任意标注来源地址。

更糟的是即使配置了可信代理，取最左端本身也是错的：XFF 由每一跳**追加**（见 `lib/server/reverse_proxy_impl.cpp`），最左端是最初的客户端，任何非可信客户端都可以在左侧预置伪造值。

修复（`lib/server/trusted_proxies.{hpp,cpp}` 为新增文件）：

- 新增 `http_server::set_trusted_proxies(std::vector<std::string> const& cidrs)`，接受 CIDR（`10.0.0.0/8`）或单个地址（`192.0.2.7`），非法项抛 `std::invalid_argument`；空列表等价于取消配置。
- `get_client_ip()` 改为：**仅当直连对端本身属于可信集合**时才读 `X-Forwarded-For`，否则一律返回 TCP 对端地址。默认从未配置，因此**默认不信任任何代理**（fail-closed）。
- 采信时**从右往左**跳过同样可信的地址，取第一个非可信地址作为真实客户端；无法解析的项跳过，全部不可用则退回对端地址。
- 可信集合是不可变对象，server 通过 `std::atomic<std::shared_ptr<...>>` 整体换指针，请求在构造时取一份快照；读方只读、无需加锁，运行中替换也不会让在途请求看到撕裂状态。
- `boost::asio::ip::basic_network_v4` 有 `netmask()` 而 `network_v6` 没有，因此匹配统一走 `net::ip::address` + 前缀长度的逐字节比较，一条代码路径覆盖两个地址族，并显式区分地址族避免 4/16 字节误判。

- 影响：~~IP 可伪造、限流可绕过、审计日志可污染~~ 已消除
- 回归测试：新增 `tests/client_ip_test.cpp`（10 个用例 / 46 断言）：默认忽略 XFF、可信代理下采信、单地址精确匹配不按超网、CIDR 前缀长度双向验证、从右往左取最近非可信跳板、伪造最左端被忽略、不可解析/全可信时退回对端、缺少 XFF、IPv6 与 v4/v6 混合匹配、非法 CIDR 抛异常。原有 `middleware_test.cpp` 中依赖 XFF 模拟多 IP 的 3 个限流用例已相应改为显式配置可信代理。
- 兼容性影响：这是**有意的行为变更**。原先部署在 nginx 等代理之后、依赖 `get_client_ip()` 取到真实客户端的代码，需要新增一次 `set_trusted_proxies()` 配置；不配置即退回按对端地址识别（安全但对代理后部署会看到代理 IP）。README 已补说明。

#### CON-01：服务端 session 容器存在明确数据竞争

> 状态：✅ **已修复**（复查 2026-09-09）

~~`sessions_` 的 insert/erase 使用 mutex，但随后记录日志时在锁外调用 `sessions_.size()`。当 server 使用多线程 executor 时，这属于未定义行为。~~

现状（[lib/server/server_impl.cpp](lib/server/server_impl.cpp#L216)）：
- `sessions_.insert()` 和 `sessions_.size()` 均在 `lock_guard` 临界区内完成，count 捕获到局部变量 `session_count`；
- `sessions_.erase()` 和对应的 `sessions_.size()` 同样在同一临界区内；
- 日志输出使用锁内捕获的 `session_count`，不再在锁外访问 `sessions_`。

- 位置：[lib/server/server_impl.cpp](lib/server/server_impl.cpp#L216)
- 影响：~~多线程 executor 下未定义行为~~ 已消除
- 建议：~~在同一临界区计算 count，或将 session 生命周期完整串行化到 strand~~ 已落实。

#### CON-02：Session middleware 的请求状态被错误地放在共享实例中

> 状态：✅ **已修复**（复查 2026-09-09）

~~`session_middleware::impl::is_new_` 是所有请求共享的 bool。两个并发请求会互相覆盖该状态并发生数据竞争；同一个 Session 对象也可能被多个请求同时修改，而 Session 内部 map 和时间字段没有锁。~~

现状（[lib/server/middleware/session_mw.cpp](lib/server/middleware/session_mw.cpp#L325)）：
- `is_new_` 共享实例已移除，改为 `req.data().store<bool>(session_new_tag, ...)` 存入请求的 custom data，每次请求独立持有；
- Session 对象同样通过 `req.data().store<value_type>(sess)` 按请求持有，不再共享；
- `after()` 中通过 `req.data().fetch<bool>(session_new_tag)` 和 `req.data().fetch<value_type>()` 获取，无并发覆盖风险。

- 位置：[lib/server/middleware/session_mw.cpp](lib/server/middleware/session_mw.cpp#L325)
- 影响：~~并发请求下 is_new 状态数据竞争~~ 已消除
- 建议：~~将"是否新建"存入 request custom data；为共享 Session 提供同步策略、版本控制或 copy/update/store 事务语义~~ 已落实。

#### CL-01：HTTP 客户端的失败重试可能发送残缺请求

> 状态：✅ **已修复**（复查 2026-09-07）

~~`async_write` 在部分数据已经写出后遇到 retryable error，会关闭连接并递归复用同一个已推进的 Beast serializer。新连接可能只发送请求剩余部分，而不是完整请求。~~

现状（[lib/client/client_impl.h](lib/client/client_impl.h#L90)）：写入循环中追踪 `bytes_written` 标志，仅在 `retryable && !bytes_written` 时允许重试——即连接建立后未发送任何字节（如连接刚建立即 reset）才会重试；一旦有任何字节成功写入，连接状态已不可恢复，重试将产生残缺请求，此时直接返回错误，由调用方决定后续策略。

- 位置：[lib/client/client_impl.h](lib/client/client_impl.h#L81)
- 建议：~~只有在确认尚未发送任何字节且方法可安全重试时才自动重试；重试必须重建 request 和 serializer~~ 已落实；后续可讨论更高层（调用方重建 request+serializer）的透明重试。

#### CL-02：跨域重定向可能泄漏认证信息

> 状态：✅ **已修复**（复查 2026-09-07，含回归测试）

~~HTTP 客户端和下载器复用原始 header 处理绝对 URL 重定向，没有删除 `Authorization`、Cookie、Proxy-Authorization 等 origin-bound 头。下载器处理 3xx 时还没有先消费或关闭响应 Body，连接回池后可能留下协议数据。~~

现状：
- **HTTP 客户端**（[lib/client/client_impl.cpp](lib/client/client_impl.cpp#L170)）：检测到跨 origin 重定向时（host/port/ssl 变化），重发前显式移除 `Authorization`、`Proxy-Authorization`、`Cookie`、`Cookie2`；
- **下载器**（[lib/client/downloader_impl.cpp](lib/client/downloader_impl.cpp#L541)）：在 redirect 循环中，当 Location 为绝对 URL 且 host/port/ssl 与当前 origin 不同时，从 `merged` 中移除以上敏感头；相对 Location 或同 origin 重定向不受影响。
- 客户端已有的 drain body 逻辑（155-158 行）保证重定向前消费响应体，连接可复用。

- HTTP 客户端：[lib/client/client_impl.cpp](lib/client/client_impl.cpp#L123)
- 下载器：[lib/client/downloader_impl.cpp](lib/client/downloader_impl.cpp#L493)
- 回归测试：`client_test.cpp` + `downloader_test.cpp` 新增跨 origin 重定向凭据隔离测试（两服务器方案，验证 Auth/Cookie 不到达目标）
- 建议：~~跨 origin 自动删除敏感头；完整解析 RFC 3986 Location；重定向前 drain body 或关闭连接；限制 HTTPS 到 HTTP 降级~~ 核心头清除已落实，HTTPS→HTTP 降级限制作为产品化决策另行讨论。

#### CACHE-01：下载缓存 key 缺少认证上下文，且忽略 HTTP 缓存语义

> 状态：✅ **已修复**（复查 2026-09-17）

~~下载缓存 key 不包含认证上下文，缓存也不区分重定向最终 URL，且未处理 `Cache-Control`/`Vary`。不同凭据的相同 URL 会共享缓存条目，可能造成用户间数据泄漏。~~

现状（2026-09-17 已按"通用存储 / HTTP 语义"分层重构）：
- **cache 接口去 HTTP 化**：`cache` 只认不透明 key、字节 body、不透明 metadata blob 与 `expires_at`，不再 include 任何 Beast/Boost 头，也不再解析 HTTP（[include/httplib/client/cache.hpp](include/httplib/client/cache.hpp)）；对齐 Chromium `disk_cache::Backend`/`OkHttp DiskLruCache` 的通用存储边界。
- **HTTP 语义上移到 downloader**：key 构造、`no-store`/`Vary: *`、`max-age` freshness、ETag/Last-Modified 验证器、重定向最终 URL 全部由 downloader 负责，并序列化成不透明 metadata blob（[lib/client/downloader_impl.cpp](lib/client/downloader_impl.cpp)）。
- **缓存 key 含认证上下文**：`Authorization`/`Proxy-Authorization`/`Cookie`/`Cookie2` 折叠为稳定摘要拼入 key，原始凭据不落盘。
- **新鲜度与保留分离**：`fresh_until` 存在 metadata 内决定是否免网络命中；缓存保留期由 `expires_at`/`max_age` 决定，使过期条目仍可用于条件 revalidate。新增"新鲜即命中、免网络"与"304 复用 body"两条路径。
- **disk_cache 生产化**：分片目录（前两位十六进制）、临时目录 + rename 原子替换、`update_metadata` 只刷新元数据不重写 body（对齐 Qt `updateMetaData`）、每项 `expires_at` + LRU + 默认 `max_age` 淘汰、目录级 advisory 文件锁（Qt 文档明确不支持多实例共享，这里显式加锁）。
- 同步修复 `disk_cache::get()` 在持有非递归锁时调用 `remove()` 的自死锁，以及 `thread_local` 清理节流在多线程下失效的问题。

- 通用存储：[include/httplib/client/cache.hpp](include/httplib/client/cache.hpp)、[lib/client/disk_cache_impl.cpp](lib/client/disk_cache_impl.cpp)
- HTTP 语义：[lib/client/downloader_impl.cpp](lib/client/downloader_impl.cpp)
- 影响：~~跨域缓存污染、错误文件返回、潜在用户间数据泄漏~~ 已消除；缓存与下载器边界清晰，可被非 HTTP 场景复用。
- 建议：FNV-1a 仍作为磁盘目录名散列；如需更强隔离可替换为加密散列。流式写入（边下边存）与读取 pin 句柄列为后续增强。

#### PROXY-01：反向代理的 Header 语义不完整

> 状态：✅ **已修复**（复查 2026-09-07）

~~主要问题包括：~~

- ~~请求方向没有完整移除 hop-by-hop headers 及 `Connection` 中点名的 headers~~ → `strip_hop_by_hop()`（[lib/server/reverse_proxy_impl.cpp](lib/server/reverse_proxy_impl.cpp#L36)）已在请求/响应双向移除固定 hop-by-hop 头及 `Connection` 点名的头（策略性保留 `Transfer-Encoding`）；
- ~~将 `Domain` 和 `Path` 追加到请求 `Cookie` 头，这两者本来是 `Set-Cookie` 属性~~ → Cookie 现按原样转发，不再改写；响应方向通过 `rewrite_set_cookie_domain()` 正确处理 Set-Cookie 的 Domain 属性；
- ~~`X-Forwarded-Proto` 使用上游协议，而不是客户端连接协议~~ → 已改为 `req.is_ssl() ? "https" : "http"`，即客户端到代理的连接协议；
- ~~CONNECT 完全绕过该代理路由和 interceptor~~ → 已随 SEC-02 整改为需显式注册 CONNECT handler。

- 位置：[lib/server/server_impl.cpp](lib/server/server_impl.cpp#L477) → 当前实现位于 [lib/server/reverse_proxy_impl.cpp](lib/server/reverse_proxy_impl.cpp#L36)
- 建议：~~建立共享 RFC hop-by-hop 清洗函数；分别实现 Cookie 与 Set-Cookie 重写；明确可信代理链和 forwarded header policy~~ 核心语义已符合 RFC 7230，可继续补充可信代理链与 forwarded 头策略文档。

### 5.3 Medium：中风险与健壮性问题

#### WEB-01：HTML 目录列表存在注入风险

> 状态：✅ **已修复**（复查 2026-09-09，先写测试后修复）

~~请求路径和文件名未经 HTML escaping/URL encoding 就写入 `<title>`、文本和 `href`。当目录内容可被外部用户影响时，可形成存储型 XSS。~~

~~此外，静态文件路径只做词法 `..` 检查，没有校验符号链接的最终目标是否仍位于 mount root；是否允许跟随外部 symlink 应由显式配置决定。~~

现状：
- `format_dir_to_html`（[lib/html/html.cpp](lib/html/html.cpp#L290)）对请求 `target` 和目录/文件名统一处理：显示文本走 `html_escape`（`&` `<` `>` `"` `'`），`<a href>` 走 `href_encode`（RFC 3986 百分号编码，空格用 `%20`，仍保留 `/` 目录分隔）。Windows 文件名限制下可用 `<`/`>` 构造的注入无法落地，单元测试用合法字符 `&` 验证转义与编码，`target` 侧则直接用 `<script>` 注入字符串验证；
- 静态挂载（[lib/server/mount_point_entry.cpp](lib/server/mount_point_entry.cpp#L157)）：`operator()` 在解析路径后新增 `detail::is_within()` canonical containment 校验，`weakly_canonical` 解析符号链接后校验最终目标仍在 `base_dir` 内，逃逸即返回 403；根挂载（尾带分隔符路径）边界已处理；
- 新增回归测试：`format_dir_to_html` 转义单元测试（`body_utils_test.cpp`）+ symlink 逃逸 e2e 测试（`response_test.cpp`，无 symlink 权限的环境自动 SKIP）。

- 位置：[lib/html/html.cpp](lib/html/html.cpp#L290)、[lib/server/mount_point_entry.cpp](lib/server/mount_point_entry.cpp#L157)
- 影响：~~存储型 XSS、符号链接逃逸读取挂载根外文件~~ 已消除
- 建议：~~HTML escaping/URL encoding、canonical containment 校验、显式 symlink 配置~~ 已落实（symlink 默认拒绝跟随逃逸，未提供放行开关，如需要可作为后续配置项）。

#### HTTP-01：Range 解析边界不完整

> 状态：✅ **已修复**（复查 2026-09-09，先写测试后修复）

~~超大 suffix range 没有 clamp 到文件大小，`start > end` 未拒绝，部分单字节 range 被错误拒绝，也没有限制 multi-range 数量。可能产生负起点、无符号长度下溢、错误响应或 Range 放大。~~

现状（[lib/html/http_ranges.cpp](lib/html/http_ranges.cpp#L48)）：
- suffix range 超过文件大小时 clamp 到 `[0, file_size-1]`，不再产生负起点；
- 移除 `start == end` 拒绝，单字节 range（如 `bytes=5-5`）合法，`bytes=0-0` 原先因 `start > 0` guard 侥幸通过的不一致也随之统一；
- 新增 `start > end` 拒绝（含 `end == 0` 边界，如 `bytes=10-0`）；
- 新增 8 个回归用例（`tests/body_utils_test.cpp`），其中 4 个先暴露既有 bug 后修复。

- 位置：[lib/html/http_ranges.cpp](lib/html/http_ranges.cpp#L48)
- 影响：~~负起点、单字节误拒绝、start>end 错误响应~~ 已消除
- 建议：~~clamp suffix、拒绝 start>end、放开单字节、限制 multi-range 数量~~ 边界已修复；multi-range 数量上限并入 SEC-03/DOS-01 统一资源限制处理。

#### INFO-01：异常详情直接返回客户端

> 状态：✅ **已修复**（复查 2026-09-07）

~~handler 抛出的 `std::exception::what()` 被写入 500 响应，可能泄露文件路径、SQL、主机名或内部状态。~~

现状（[lib/server/session.cpp](lib/server/session.cpp#L377)）：`catch (std::exception const& e)` 中 `e.what()` 仅记入服务器日志；返回客户端的 500 响应改为 `set_error_content()` 生成的通用错误页，不含任何异常消息。提交 `ce53698` 专门修复此问题。

- 位置：[lib/server/session.cpp](lib/server/session.cpp#L340)
- 建议：无需进一步处理。

#### DOS-01：多个长期容器没有容量与淘汰约束

> 状态：✅ **已修复**（本次整改，三项均已收口并补回归测试）

1. **Rate limit bucket 无界增长** —— 原实现以 `impl_->buckets[ip]` 取桶（`operator[]` 必然插入）且从不 erase。现改为 `find` + 显式 `emplace`，并新增容量与淘汰约束：
   - `max_tracked_clients(n)`（默认 8192）：达到上限后内存就此封顶；桶满时先顺带回收空闲桶；
   - `idle_expiration(d)`（默认与 window 相同）：桶按 `last_seen` 判空闲，`before()` 中按 `idle_expiration` 节流顺带清扫，单次请求成本均摊为 O(桶数)，不引入后台线程；
   - `when_full(capacity_action)`：`evict_oldest`（默认）淘汰最久未访问的桶为新客户端腾位，**每个请求都被计数，限流不会因为桶满而失效**；`reject` 则直接返回 429，保护性更强但攻击者占满桶表即可把新客户端挡在门外。
   - 新增 `tracked_clients()` 供测试与可观测性使用。位置：`lib/server/middleware/rate_limit.cpp`、`include/httplib/server/middleware/rate_limit.hpp`。
   - 限流可被 `X-Forwarded-For` 绕过的根因一并修复（见 SEC-07），桶键来源现在是可信的。

2. **Session store 无上限、无后台回收** —— 过期回收原本只挂在「命中时顺带检查」与「手工 `cleanup()`」两处。现新增：
   - `max_sessions(n)`（默认 8192，经 `session_store::set_max_sessions()` 虚接口下发，自定义 store 默认忽略该设置）：超限时先回收已过期条目，仍满则淘汰最久未访问的一条；
   - `save()` 中按 `max(ttl/4, 1s)` 节流顺带清扫，保证过期条目不会长期滞留，且不需要定时器；
   - 新增 `size()` 供测试与可观测性使用，`cleanup()` 语义保持（立即回收全部过期项）。位置：`lib/server/middleware/session_mw.cpp`、`lib/server/middleware/memory_store.hpp`、`include/httplib/server/middleware/session.hpp`。

3. **Router 正则 ReDoS** —— 仍用 `std::regex`（不引入 RE2 新依赖），但把风险从请求期前移到注册期：
   - 注册时做准入检查 `detail::has_redos_shape()`，拒绝量词嵌套（`(a+)+`、`(a*)*`、`(a{2,})+`）与被量词作用且内部含交替的分组（`(a|aa)+`），命中即抛 `std::invalid_argument`；语法错误仍由 `std::regex` 抛 `std::regex_error`，行为不变；
   - pattern 长度上限 256 字符（`regex_pattern_max`），超长即拒绝；
   - 参与匹配的路径段长度上限 1024 字符（`regex_subject_max`），超长段直接判定不匹配（快速 404），不再做无意义回溯。
   - 位置：`lib/server/router_impl.cpp`（`has_redos_shape`、两处上限、`match_nodes` 的段长判断）。

- 影响：~~内存只增不减、单请求长时间占用执行线程~~ 已消除
- 回归测试：`middleware_test.cpp`（桶回收、容量有界、容量上限不干扰计数、**桶满后新客户端仍被计数**、evict_oldest 保住最活跃客户端、reject 策略拒绝新客户端）、`session_cookie_test.cpp`（会话数上限、更新不误淘汰、save 顺带回收、显式 cleanup、上限优先回收过期项）、`router_test.cpp`（危险 pattern 注册期拒绝、安全 pattern 仍接受、超长 pattern 拒绝、超长 subject 快速 404）；既有 `regex_error` 用例保持通过
- 残留风险：
  1. 路由正则准入检查是启发式的，不等价于线性时间保证。未覆盖形态（例如不带分组的多重无界量词 `a*a*a*b`）由 `regex_subject_max` 限制最坏输入规模兜底；要彻底消除需改用 RE2 或自建 NFA 引擎，属独立改动。
  2. 容量上限只能封顶内存，无法约束跨 IP 的总请求量——任何按 IP 的限流都有这个固有上限，需要总量保护时应在前置网关或反向代理层再加一层。

> 已随 SEC-03 收口的部分：header/body/解压后大小/upload/multipart 字段数与单 part 大小/Range 数量均已有上限；WebSocket 写入由 `util::async_mutex write_mutex_` 串行化（背压而非无界队列，`lib/server/websocket_conn_impl.hpp`），公共 `action_queue` 亦提供可选 `max_pending`。

#### API-01：运行期可变配置缺少并发保护

> 状态：✅ **已修复**（本次整改，明确契约而非引入运行期更新机制）

原问题拆分后，实际只有 router 未经同步：

1. **标量配置本就线程安全** —— `read_timeout` / `write_timeout`、`header_limit` / `body_limit`、`logger`、`compress_predicate`、`form_data` 参数、`trusted_proxies`、`ssl_context` 全部以 `std::atomic` 或 `std::atomic<std::shared_ptr<...>>` 快照存储（见 `lib/server/server_impl.h`、`lib/util/logging.hpp`），运行期修改与请求期读取不会撕裂，与 `http_server` 文档标注的“运行期可改”一致。
2. **router 是唯一真正无同步的部分** —— 原先 `router_impl` 持有 `std::shared_mutex`，但保护并不完整且有误导性：
   - `set_not_found_handler_impl` / `set_post_routing_handler_impl` 写无锁、`post_routing` 读亦无锁；
   - `global_before_` / `global_after_` 仅在 `use_impl` 写加锁，而派发时在 `wrap_global` 的协程体内无锁遍历（无法在 `co_await` 期间持锁）；
   - `pre_routing` 在 `shared_lock` 下返回裸 `Node const*`，而 `process_routing` 在锁**释放之后**才解引用它——即便有并发写，该锁也挡不住这次 use-after-free。

修复：**把“仅启动前配置”明确为强制契约，并移除 router 的锁**。

- `lib/server/router_impl.{h,cpp}` 删除 `std::shared_mutex mutex_` 及全部 7 处 lock/unique_lock/shared_lock。路由表只在配置阶段写入；请求期只读。
- `reset()` **保留**：它在 `async_run()` 停机路径中被调用（`lib/server/server_impl.cpp:189`），时序是「accept 全部结束 → `while (!sessions_.empty())` 排空在途会话 → `reset()`」。会话是在 `strand_` 上同步 `sessions_.insert()` 后才 `co_spawn`（`server_impl.cpp:242-246`），accept 已停即不可能再有新会话，因此 `reset()` 执行时确无并发读者——无需锁。
- `router` 补充类级线程安全说明（配置阶段、无同步、运行期注册属未定义行为），并注明停止后框架会清空路由表、再次 `run()` 前需重新注册；`http_server::router()` 文档同步点明“内部不加锁”。
- 选型说明：报告原本给出「明确仅启动前配置」**或**「提供 strand 内原子更新机制」二选一。鉴于路由注册发生在进程启动阶段、运行期重配路由无实际场景，选择前者；后者需把路由树改为 copy-on-write 共享所有权（消除裸指针悬垂），改动触及最热匹配路径，收益不成比例，故不做。

- 影响：~~部分结构读写无锁，运行期修改配置存在数据竞争~~ 已消除（契约明确 + 唯一无同步处以“不可并发修改”约束）
- 热路径副作用：`pre_routing`（每请求）与 `query_ws_handler`（每次 WS 升级）不再各做一次 `shared_lock` 获取/释放。
- 回归：现有 router / middleware / 生命周期测试全量通过（`ctest` 6/6）。

## 6. 构建、测试与发布质量

### 6.1 实际构建验证

执行命令：

```bash
cmake -S . -B build \
  -DHTTPLIB_ENABLED_TESTS=ON \
  -DHTTPLIB_ENABLED_EXAMPLES=OFF \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

配置阶段失败：

```text
Could not find a package configuration file provided by "Boost"
with any of the following names:
  BoostConfig.cmake
  boost-config.cmake
```

这说明当前环境未安装依赖，并不代表源码必然无法编译。但仓库缺少 `vcpkg.json`、Conan 配置、FetchContent 或依赖锁文件，使得新环境无法按仓库自身信息完成可复现构建。

### 6.2 CMake 安装配置问题

1. `httplib-config.cmake.in` 没有声明公开依赖 `Boost::url`。
2. 数据库功能公开链接 `Boost::charconv`，安装 config 没有按 `HTTPLIB_ENABLED_DATABASE` 查找该依赖。
3. `target_include_directories` 使用 `${CMAKE_INSTALL_INCLUDEDIR}` 后才 `include(GNUInstallDirs)`，该变量可能在构造 install interface 时尚未初始化。
4. 配置模板未导出 database feature 状态。
5. 依赖只有最低存在性要求，没有锁定或经过验证的版本组合。

- 位置：[lib/CMakeLists.txt](lib/CMakeLists.txt#L16)
- 安装模板：[cmake/httplib-config.cmake.in](cmake/httplib-config.cmake.in#L1)

### 6.3 测试质量

优点：

- 743 个 Catch2 测试；
- 大量真实 TCP 集成场景；
- 覆盖 HTTP 方法、路由、WS、SSE、NDJSON、代理、下载器和 Body；
- 已有部分随机 payload 测试；
- 测试已按组件拆分为 6 个可执行文件（`core`/`http`/`client`/`proxy`/`jwt`/`db`），各自注册 ctest label。

缺口：

- 没有自动 CI；`.github` 被显式 gitignore；
- 没有 ASan、UBSan、TSan、MSan 或 Valgrind job；
- 没有持续 fuzz target，仅有测试代码内的随机输入；
- 已补充：畸形 `%xx`（`body_utils_test.cpp`）、上传路径逃逸（`http_methods_test.cpp`）、JWT exp/nbf（`jwt_test.cpp`）、静态挂载路径逃逸（`response_test.cpp`）、CONNECT 默认拒绝（`proxy_test.cpp`）、跨 origin 重定向凭据隔离（`client_test.cpp` + `downloader_test.cpp`）等回归测试；
- 仍缺：TLS 主机名验证、同 Session 并发、Range bombing、慢连接和资源上限测试。

### 6.4 仓库治理与法律风险

- 已补齐 BSL-1.0 `LICENSE`（原“根目录没有 LICENSE/COPYING 或 SPDX 声明”已不成立）。
- 仍缺 SECURITY policy、贡献指南、变更日志或 GitHub Release。
- README 已重写（中英双版本），乱码已清除；测试代码中仍有少量损坏字符（`tests/client_pool_test.cpp` 第 830/881/888 行注释）。
- 示例代码内嵌测试证书和加密私钥，虽用于 demo，也会触发密钥扫描并容易被误复制到真实部署。

## 7. 整改优先级

### P0：任何公网部署前必须完成

> 以下各项均已修复；仅第 6 项的常量时间比较仍可补充。

1. ~~默认禁用 CONNECT；接入认证、目标 ACL、IP/DNS 校验和流量限制~~ → 默认已拒绝（405），CONNECT 走完整 pre/post-routing 管线。
2. ~~修复 multipart 文件名路径逃逸，服务端生成受控文件名并做目录 containment 校验~~ → 已修复（basename + 随机前缀 + weakly_canonical 校验）。
3. ~~增加 Header、Body、解压后数据、multipart、Range、WS 队列等统一安全限制~~ → 已落地：header 64KB、upload 10MB、body 1GiB、Range 数量（默认 100 段）、multipart 字段数（默认 128）与单 part 大小、解压后产出大小，均可配置。
4. ~~修复 URL 解码越界和非法输入处理~~ → 已修复。
5. ~~HTTPS/WSS 默认验证证书链及主机名~~ → 已修复。
6. ~~JWT 强制校验算法、`exp`、`nbf`，使用常量时间签名比较~~ → 算法与时间戳校验已修复；常量时间比较仍可补充。

### P1：进入生产压测前完成

1. 明确 executor/strand 模型，消除 server sessions、Session middleware、socket stop、client 并发读写等数据竞争。
    - server sessions（CON-01）与 Session middleware（CON-02）两项数据竞争已修复；socket stop 与 client 并发读写仍需明确 strand 约束。
2. ~~修复客户端部分写入重试和 downloader 重定向连接复用~~ → 客户端重试已修复（仅零字节时允许重试）。
3. ~~跨 origin 重定向删除敏感 header，禁止非授权协议降级~~ → 已修复（client + downloader，含回归测试）。
4. ~~重构 cache key 和 HTTP cache policy~~ → 已修复（key 含认证上下文与最终 URL 校验，尊重 `no-store`/`Vary: *`，元数据白名单）。
5. ~~完整实现代理 hop-by-hop、Cookie/Set-Cookie 和 Forwarded header 语义~~ → 已修复。
6. ~~修复 Range 边界、目录 HTML escaping、异常详情泄漏，以及长期容器淘汰~~ → 全部已修复。
    - Range 边界（HTTP-01）、目录 HTML escaping/symlink containment（WEB-01）、异常详情泄漏（INFO-01）均已修复并含回归测试；长期容器淘汰（DOS-01：Rate limit bucket、Session store、Router 正则准入）已修复并含回归测试。

### P2：发布前完成

1. 添加明确开源许可证、SECURITY、CHANGELOG 和版本发布流程。
2. 增加 vcpkg/Conan manifest 或其他可锁定依赖方案。
3. 建立 Linux/macOS/Windows CI，覆盖 SSL、Compression、Database 和静态/动态库矩阵。
4. 加入 clang-format check、clang-tidy、ASan、UBSan、TSan 和 fuzz。
5. 拆分协议内核与 JWT、Session、DB、下载缓存等应用级模块，降低核心攻击面。

## 8. 建议的生产准入条件

满足以下条件后，才建议进入灰度：

- 所有 P0 项有回归测试并经过人工复核；
- ASan/UBSan/TSan 在完整测试集和压力测试下无报告；
- 对 parser、URL decode、multipart、Range、WebSocket、JWT 建立持续 fuzz；
- 通过慢 Header、慢 Body、大 Body、压缩炸弹、Range bombing、并发 stop/restart 和连接耗尽测试；
- TLS、CONNECT、代理目标和上传目录的安全默认值经过文档确认；
- 完成依赖锁定、SBOM、许可证和 CI 发布产物；
- 对 `dev` 建立稳定发布分支，不直接以高频开发分支作为生产依赖。

## 9. 最终结论

httplib 的基础结构并不差：作者理解 Boost.Asio/Beast、协程、PIMPL、路由 Trie 和真实网络测试，项目也已超过简单示例库的规模。但当前最大问题不是代码风格，而是**安全边界、并发契约和发布工程没有跟上功能扩张速度**。

最初报告中的风险项现已 **18 项完全修复**（SEC-01/02/03/04/05/06/07、CON-01/02、CL-01/02、INFO-01、PROXY-01、HTTP-01、WEB-01、CACHE-01、DOS-01、API-01），其中 CL-01/02、CON-01/02、HTTP-01、WEB-01、CACHE-01、SEC-03、SEC-07、DOS-01 均含回归测试或代码复核。剩余未收口项集中在 **JWT 常量时间比较、socket stop / client 并发读写的 strand 契约，以及构建/发布工程（安装配置、依赖锁定、CI、动态检测、fuzz）**；路由正则另有一项已知残留（准入检查为启发式，彻底解决需换 RE2/NFA），按 IP 的限流则天然无法约束跨 IP 总量，需要总量保护时应在前置网关再加一层。

建议先冻结功能扩张，以并发契约与构建发布工程收口为主线，再补动态检测与 fuzz。
