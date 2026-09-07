# httplib 架构、代码质量与风险分析报告

> 审查日期：2026-08-09  
> 审查分支：`dev`  
> 审查提交：`56f47cf811254b90057239360b499f9b2f09efca`（修复卡死bug）  
> 上游仓库：<https://github.com/yansong1221/httplib>  
> 复查日期：2026-09-07  
> 复查提交：`6f8a602`（HEAD，逐一核对各风险项修复状态）

## 1. 执行摘要

该项目是一套基于 Boost.Asio/Beast、面向 C++23 的异步 HTTP/1.1 与 WebSocket 客户端/服务端框架，同时包含路由、中间件、反向代理、文件服务、SSE、NDJSON、JWT、Session、下载器、磁盘缓存和可选数据库支持。

总体判断：**架构方向合理、功能覆盖完整、测试投入较明显。截至 2026-09-07 复查，报告中的安全阻断项（上传路径逃逸、TLS/JWT 校验、URL 解码越界等）已大部分修复；但 body 默认上限、跨域重定向敏感头、并发数据竞争和发布工程仍未收口，不建议未经整改直接暴露在公网或承担认证、上传、代理等关键业务。**

| 维度 | 评分 | 结论 |
|---|---:|---|
| 架构设计 | 6.5/10 | 分层和核心抽象合理，但职责范围过宽 |
| 模块化与可读性 | 6/10 | 目录清楚，部分中心文件过大、耦合偏重 |
| 测试建设 | 7/10 | 273 个真实 TCP 测试，但安全和并发边界覆盖不足 |
| 并发可靠性 | 4/10 | 仍存在数据竞争，线程模型约束不清晰 |
| 安全性 | 5/10 | 上传路径逃逸、TLS/JWT、URL 解码等已修复；body 上限、敏感头重定向等仍待整改 |
| 构建与发布成熟度 | 3.5/10 | 缺依赖锁定、CI、许可证，安装配置不完整 |

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
- Catch2 `TEST_CASE` 共 273 个，大量测试会启动真实 TCP 服务进行端到端验证。
- 公共头文件 55 个，带导出标记的公共类/结构约 46 个，API 面已经比较大。
- 当前默认克隆分支为 `dev`；`master` 比 `dev` 落后 159 个提交。
- 当前提交比 `v1.0.5` 标签多 52 个提交；2026-07-30 至 2026-08-08 约有 111 个提交，代码仍在快速演进。
- 362 个历史提交中约 149 个提交信息仅为 `update`，问题定位和变更追溯成本较高。

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

2. **PIMPL 未真正隐藏 Boost**

   55 个公共头文件中约 39 个仍直接暴露 Boost.Asio、Beast、JSON、Boost.System 或 spdlog 类型。调用方依然必须接受 Boost 的源码、ABI、编译时间和版本耦合。

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

> 状态：⚠️ **部分修复**（复查 2026-09-07）

~~普通 HTTP 会话在执行 `router.pre_routing()` 前直接判断 `CONNECT`，随后解析目标并建立任意 TCP 连接。现有路由鉴权、JWT、Rate Limit 和自定义 ACL 均不会执行。~~

现状：
- CONNECT 默认被拒绝：未注册 `set_connect_handler` 时返回 `405 method_not_allowed`，不再是开放代理；
- 已支持路径匹配（`query_connect_handler`）和全局中间件（`use()` 经 `wrap_global` 包装）；
- 但仍绕过通用 `pre_routing`/`post_routing` 管线：路由级 `Aspects` 中间件、`not_found_handler`、`post_routing_handler` 均不执行，认证/ACL 仍需依赖 CONNECT handler 内自行实现。

- 入口：[lib/server/session.cpp](lib/server/session.cpp#L234)
- 转发实现：[lib/server/session.cpp](lib/server/session.cpp#L475)
- 影响：开放代理已消除；但 CONNECT 与其他方法的鉴权、限流模型尚未统一
- 建议：~~默认禁用 CONNECT；提供显式开关；必须经过认证、目标主机/端口 ACL、DNS/IP 二次校验和连接/流量限制~~ 让 CONNECT 走完整 pre/post-routing 管线或明确文档化其 handler 内的认证/ACL 责任。

#### SEC-03：请求 Header/Body 基本不设上限

> 状态：⚠️ **部分修复**（复查 2026-09-07）

~~服务端将 header limit 设置为 `uint32_t` 最大值，body limit 设置为 `unsigned long long` 最大值。普通字符串、表单和 JSON 均可能消耗接近无限内存，且公共 server API 没有全局限制入口。~~

现状：
- header limit 已改为合理默认值 `65536`（64 KB），并新增 `set_header_limit()` 配置入口；
- upload file limit 默认 10 MB，同样可配置；
- 但 `body_limit_` 原默认 `std::numeric_limits<std::uint64_t>::max()`（无限），存在内存耗尽风险；复查后已将服务端默认改为 **1 GiB**（`[lib/server/server_impl.h](lib/server/server_impl.h#L167)`），multipart 字段数、Range 数量、解压后大小等仍无上限。

- 请求解析：[lib/server/session.cpp](lib/server/session.cpp#L207)，默认值 [lib/server/server_impl.h](lib/server/server_impl.h#L166)
- 公共 API：[lib/server/server.hpp](lib/server/server.hpp#L65)
- JSON 分配：[lib/body/json_body.cpp](lib/body/json_body.cpp#L55)
- 影响：header/文件上传已受限；**body limit 默认为无限**仍是 DoS 风险
- 建议：~~为 header、总 body、各 body 类型、multipart 字段数/字段大小、单文件和总上传量设置安全默认值及可配置上限；限制解压后的大小~~ body limit 应设安全默认值（如 10~100 MB）；补齐 multipart 字段数/字段大小、Range 数量与解压后大小上限。

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

现状（[lib/util/jwt.cpp](lib/util/jwt.cpp#L482)）：
- 校验 Header 中 `alg` 是否与配置算法一致（不一致报 `algorithm_mismatch`）；
- 校验 `exp`（超时返回 `expired`）与 `nbf`（未生效返回 `not_yet_valid`），均支持 `with_clock_skew()` 容忍时钟偏差；
- 保留 iss/sub/aud/jti 及自定义 claim 校验，失败即返回对应 error_code。

- 位置：[lib/util/jwt.cpp](lib/util/jwt.cpp#L469)
- 影响：~~过期或提前使用的 Token 可继续访问受保护资源~~ 已消除
- 建议：~~默认强制校验 `exp`/`nbf`，支持 clock skew；核对 `alg`；使用常量时间签名比较；增加缺失/类型错误 claim 的安全失败路径~~ 时间与算法校验已落实，可补充常量时间比较与缺失 claim 的显式测试。

### 5.2 High：高风险项

#### SEC-06：URL 解码对畸形 `%xx` 存在越界读取

> 状态：✅ **已修复**（复查 2026-09-07）

~~`url_decode` 在看到 `%` 后直接执行两次 `++r`，没有验证剩余长度，也不校验字符是否为十六进制。结尾 `%`、`%A` 等输入可能触发未定义行为。请求路径创建时会自动调用该函数，因此可由远程请求触发。~~

现状（[lib/util/misc.cpp](lib/util/misc.cpp#L36)）：
- 仅在 `r + 2 < size` 且 `is_hex_digit(str[r+1])` 且 `is_hex_digit(str[r+2])` 时才解码；
- 短路求值保证越界访问不可能发生；畸形 `%` 按普通字符原样保留，不会触发未定义行为。

- 解码实现：[lib/util/misc.cpp](lib/util/misc.cpp#L31)
- 请求调用点：[lib/server/request_impl.hpp](lib/server/request_impl.hpp#L26)
- 建议：~~解析前检查 `r + 2 < size`，严格验证 hex digit；非法输入返回 error/result，不应静默解码~~ 越界与非法输入已安全处理。可选：对畸形输入是否改为显式报错仍有产品化讨论空间。

#### CON-01：服务端 session 容器存在明确数据竞争

`sessions_` 的 insert/erase 使用 mutex，但随后记录日志时在锁外调用 `sessions_.size()`。当 server 使用多线程 executor 时，这属于未定义行为。

- 位置：[lib/server/server_impl.cpp](lib/server/server_impl.cpp#L260)
- 建议：在同一临界区计算 count，或将 session 生命周期完整串行化到 strand。

#### CON-02：Session middleware 的请求状态被错误地放在共享实例中

`session_middleware::impl::is_new_` 是所有请求共享的 bool。两个并发请求会互相覆盖该状态并发生数据竞争；同一个 Session 对象也可能被多个请求同时修改，而 Session 内部 map 和时间字段没有锁。

- 位置：[lib/server/middleware/session_mw.cpp](lib/server/middleware/session_mw.cpp#L199)
- 建议：将“是否新建”存入 request custom data；为共享 Session 提供同步策略、版本控制或 copy/update/store 事务语义。

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

#### CACHE-01：下载缓存 key 不包含 origin 和认证上下文

虽然 cache 接口参数名为 URL，下载器实际只用 `ui.path` 查询和写入缓存。不同 scheme、host、port 的相同路径会共享缓存；缓存也不区分 Authorization/Cookie/Vary。

- 查询：[lib/client/downloader_impl.cpp](lib/client/downloader_impl.cpp#L444)
- 主入口：[lib/client/downloader_impl.cpp](lib/client/downloader_impl.cpp#L1048)
- 影响：跨域缓存污染、错误文件返回、潜在用户间数据泄漏。
- 建议：key 至少包含规范化 scheme/host/port/path/query；正确处理 `Cache-Control`、`Vary`、认证请求和重定向最终 URL；避免使用 FNV-1a 作为不可信隔离边界。

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

请求路径和文件名未经 HTML escaping/URL encoding 就写入 `<title>`、文本和 `href`。当目录内容可被外部用户影响时，可形成存储型 XSS。

- 位置：[lib/html/html.cpp](lib/html/html.cpp#L153)

此外，静态文件路径只做词法 `..` 检查，没有校验符号链接的最终目标是否仍位于 mount root；是否允许跟随外部 symlink 应由显式配置决定。

#### HTTP-01：Range 解析边界不完整

超大 suffix range 没有 clamp 到文件大小，`start > end` 未拒绝，部分单字节 range 被错误拒绝，也没有限制 multi-range 数量。可能产生负起点、无符号长度下溢、错误响应或 Range 放大。

- 位置：[lib/html/http_ranges.cpp](lib/html/http_ranges.cpp#L8)

#### INFO-01：异常详情直接返回客户端

> 状态：✅ **已修复**（复查 2026-09-07）

~~handler 抛出的 `std::exception::what()` 被写入 500 响应，可能泄露文件路径、SQL、主机名或内部状态。~~

现状（[lib/server/session.cpp](lib/server/session.cpp#L377)）：`catch (std::exception const& e)` 中 `e.what()` 仅记入服务器日志；返回客户端的 500 响应改为 `set_error_content()` 生成的通用错误页，不含任何异常消息。提交 `ce53698` 专门修复此问题。

- 位置：[lib/server/session.cpp](lib/server/session.cpp#L340)
- 建议：无需进一步处理。

#### DOS-01：多个长期容器没有容量与淘汰约束

- Rate limit buckets 会随新 IP 增长，不主动清理；
- 默认 Session store 只在命中特定 session 或手工 cleanup 时清理；
- WebSocket/action queue 没有最大消息数和最大字节数；
- Router 正则由 `std::regex` 执行，复杂表达式可能造成高 CPU；
- Header、Range、multipart 字段数没有合理上限。

#### API-01：运行期可变配置缺少并发保护

Router 注册、not-found/post handler、logger、timeout、压缩 predicate 等 setter 看起来可以随时调用，但部分读写没有锁或只保护了部分结构。应明确“仅启动前配置”，或提供 executor/strand 内的原子配置更新机制。

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

- 273 个 Catch2 测试；
- 大量真实 TCP 集成场景；
- 覆盖 HTTP 方法、路由、WS、SSE、NDJSON、代理、下载器和 Body；
- 已有部分随机 payload 测试。

缺口（复查 2026-09-07 更新）：

- 没有自动 CI；`.github` 被显式 gitignore；
- 没有 ASan、UBSan、TSan、MSan 或 Valgrind job；
- 没有持续 fuzz target，仅有测试代码内的随机输入；
- 已补充：畸形 `%xx`（`body_utils_test.cpp`）、上传路径逃逸（`http_methods_test.cpp`）、JWT exp/nbf（`jwt_test.cpp`）、静态挂载路径逃逸（`response_test.cpp`）、CONNECT 默认拒绝（`proxy_test.cpp`）等回归测试；
- 仍缺：TLS 主机名验证、跨域重定向凭据、同 Session 并发、Range bombing、慢连接和资源上限测试；
- 目前所有测试集中到一个可执行文件，不利于按组件并行和隔离失败。

### 6.4 仓库治理与法律风险

- 根目录没有 `LICENSE`、`COPYING` 或 SPDX 声明。即使仓库公开，也不能自动推断获得复制、修改和分发授权；这是外部采用的法律阻断项。
- 没有 SECURITY policy、贡献指南、变更日志或 GitHub Release。
- README 和部分测试注释包含损坏字符 `�?`。
- 示例代码内嵌测试证书和加密私钥，虽用于 demo，也会触发密钥扫描并容易被误复制到真实部署。

## 7. 整改优先级

### P0：任何公网部署前必须完成（截至 2026-09-07 复查）

> 以下 1~2、4~6 项在复查提交上已修复并应补充回归测试；3 项为仍待处理的核心项。

1. ~~默认禁用 CONNECT；接入认证、目标 ACL、IP/DNS 校验和流量限制~~ → 默认已拒绝（405），仍需接入认证与目标 ACL。
2. ~~修复 multipart 文件名路径逃逸，服务端生成受控文件名并做目录 containment 校验~~ → 已修复（basename + weakly_canonical 校验）。
3. **增加 Header、Body、解压后数据、multipart、Range、WS 队列等统一安全限制** → header 64KB、upload 10MB、body 1GiB 等默认值已落地；multipart 字段数/字段大小、Range 数量、WS 队列上限仍缺失。
4. ~~修复 URL 解码越界和非法输入处理~~ → 已修复。
5. ~~HTTPS/WSS 默认验证证书链及主机名~~ → 已修复。
6. ~~JWT 强制校验算法、`exp`、`nbf`，使用常量时间签名比较~~ → 算法与时间戳校验已修复；常量时间比较仍可补充。

### P1：进入生产压测前完成

1. 明确 executor/strand 模型，消除 server sessions、Session middleware、socket stop、client 并发读写等数据竞争。
2. ~~修复客户端部分写入重试和 downloader 重定向连接复用~~ → 客户端重试已修复（仅零字节时允许重试）。
3. ~~跨 origin 重定向删除敏感 header，禁止非授权协议降级~~ → 已修复（client + downloader，含回归测试）。
4. 重构 cache key 和 HTTP cache policy。
5. ~~完整实现代理 hop-by-hop、Cookie/Set-Cookie 和 Forwarded header 语义~~ → 已修复。
6. 修复 Range、目录 HTML escaping、~~异常详情泄漏~~ 和长期容器淘汰。

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

截至 2026-09-07 复查：最初报告中的 17 项风险已有 6 项完全修复（SEC-01/04/05/06、INFO-01、PROXY-01），SEC-02/CONNECT 部分修复，SEC-03 部分修复（header 已限、body 仍无限）。剩余生产阻断项集中在 **body 默认上限缺失、跨域重定向敏感头、并发数据竞争（CON-01/02、API-01）、客户端部分写入重试、缓存 key 与 Range 边界**。

建议先冻结功能扩张，以 body 上限、线程模型收口和并发/重定向安全整改为主线，再补动态检测、fuzz 和构建发布工程；随后进入生产压测前再处理缓存与 Range 等健壮性项。
