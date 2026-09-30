# 更新日志

本文件记录 `httplib` 的重要变更。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 已知问题

- **路由正则的 ReDoS 检查是启发式的。** 注册期会拦截嵌套量词（`(a+)+`）与被量词作用且含交替的
  分组（`(a|aa)+`），但不覆盖不带分组的连续无界量词（`a*a*a*b`）。这些情况由
  `regex_subject_max`（单路径段 1024 字符）限制最坏输入规模兜底，不保证线性时间匹配。
  彻底解决需改用 RE2 或自建 NFA 引擎。pattern 属于部署者代码而非外部输入，故此项为自伤风险
  而非可远程利用的漏洞。
- **按 IP 的限流无法约束跨 IP 的请求总量。** 任何按来源 IP 限流的方案都有这个固有上限；
  需要总量保护时应在服务端之前加一层网关。

## [1.0.5] — 未发布

首个计划发布的版本。`CMakeLists.txt` 中的 `project(httplib VERSION 1.0.5)` 已就位，但尚未打
tag；以下变更相对仓库初始状态累积。

### 安全

以下问题经 `ANALYSIS_REPORT.md` 审计发现并修复，每项均含回归测试或代码复核。

- **上传路径逃逸**：multipart 落盘文件名改为 basename + 随机前缀，并做目录 containment 校验
  （防止同名上传互相覆盖与 `../` 逃逸）。
- **JWT 校验**：强制算法白名单（拒绝 `alg` 混淆/降级），校验 `exp` / `nbf` 并支持 clock skew，
  签名比较改为常量时间（消除 timing side channel）。
- **URL 解码越界**：`%xx` 解码的边界与非法输入处理。
- **Range 边界**：畸形 `Range` 头不再越界读。
- **目录列表注入**：目录 HTML 输出做 escaping，并对符号链接做 containment 约束。
- **CONNECT 开放代理**：默认拒绝 CONNECT（405），启用时走完整 pre/post-routing 管线。
- **资源上限（DoS）**：统一 header / body / 解压后大小 / multipart 字段数与单 part 大小 /
  Range 段数上限；解压限额只约束**解压产出**，不会被压缩长度预检绕过。
- **长期容器淘汰**：限流桶与会话存储增加容量上限、空闲清扫与最久未访问淘汰（默认策略为
  淘汰最旧，可选拒绝新客户端）。
- **客户端 IP 伪造**：`get_client_ip()` 默认不再采信 `X-Forwarded-For`；新增
  `set_trusted_proxies()`，仅对声明的可信代理网段从右向左解析 XFF。
- **跨域重定向**：删除敏感 header，禁止向未授权协议降级；下载器校验完整性并修复重定向连接复用。
- **异常详情泄漏**：错误响应不再回传内部异常细节。
- **下载缓存污染**：cache key 纳入 scheme/host/port 与认证上下文，校验最终 URL，尊重
  `no-store` / `Vary: *`。
- **客户端部分写入**：仅在零字节写入时允许重试。

### 修复

- **并发**：session 绑定 per-connection strand 并串行化 I/O 与 abort；连接池 teardown 改为
  先清空池状态再关连接；修复流式读取的连接级并发竞争。
- **路由**：移除 `router_impl` 的 `shared_mutex`——该锁保护不完整（not-found/post handler
  读写皆无锁、派发时无锁遍历中间件、`pre_routing` 返回的裸指针在锁释放后仍被解引用），
  改为明确「仅启动前配置」契约。
- **正则准入**：注册期拦截灾难性回溯形态，限制 pattern（256 字符）与路径段（1024 字符）长度。
- **WebSocket**：写入由背压式 mutex 串行化，不再使用无界队列。
- **流式读取**：NDJSON reader 不再丢末条记录；压缩 SSE / NDJSON 尾部不再被截断；HEAD 保留
  `Content-Length`；零长缓冲读空 body 不再空转。
- **下载器**：修复多段续传、Range 回退与进度计数。

### 变更

- **公共 API 不再暴露 Boost.Beast 类型**（`880f686`），Beast 别名移入私有
  `lib/beast_alias.hpp`。
- **Body 子系统重构**：`body_state` 取代原 `any_body::value_type`，按 `Content-Type` 选择
  body 类型，提供 `as_string()` / `as_json()` / `as_form_data()` / `as_query_params()`
  类型化访问器。
- **枚举转换**改为 X-macro 名字表；`fields()` 改为生成器，header 改为 PIMPL 借用视图。
- **削减公共头的 Boost 传递依赖**；`ticker` 改用 PIMPL。
- **新增中间件**：限流、Session、JWT 认证、DB 等。中间件改为「切面」形态：作为
  `set_http_handler` 的尾随可变参数传入，或经 `router::use()` 全局注册，可提供
  `before()` / `after()`。

### 文档

- `THREAD_MODEL.md`：完整 strand / mutex 拓扑与并发契约。
- `README.md`（中文）与 `README.en.md`（英文）互链。
- `SECURITY.md`：漏洞报告渠道与部署者责任清单。

### 已知不兼容变更

从早期版本迁移时需注意（项目此前未发布正式版本，此处记录以固定当前 API）：

- `request::merge()` 更名为 `request::replace()`。
- `router::use()` 的中间件改为「切面」形态，不再是简单的请求处理器。
- `any_body::value_type` 移除，改用 `body_state` 与类型化访问器。
- 公共头不再包含 `boost/beast/*`。

## [1.0.5] — 链接

- 许可证：[BSL-1.0](LICENSE)
