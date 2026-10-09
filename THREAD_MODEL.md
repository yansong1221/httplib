# 线程与 Strand 模型

本文汇总服务端、客户端、连接池、WebSocket 与下载调度器的并发约束。目的是让「哪段代码在哪个
executor 上跑、哪些状态归谁独占、哪些操作必须串行」有一处可查的说明，而不是散落在各头文件
的注释里。

阅读顺序建议：先看 [1. 三个基本约定](#1-三个基本约定)，再按需要跳到对应组件。

---

## 1. 三个基本约定

### 1.1 协程不等于串行

`co_await` 会让出执行权。**两个 `co_spawn` 出去的协程即使跑在同一个 strand 上，也是交错的**：
它们在 `co_await` 点互相穿插。strand 提供的保证是「不会同时执行」，不是「不会交错」。

因此本项目里凡是「必须连续执行、不能被打断」的状态，都用 mutex 或 atomic 保护，而不是依赖
strand。反过来，凡是「可以交错、但不能同时」的（如 socket 读写），用 strand 就够了。

### 1.2 socket 操作的串行化靠 strand，不靠 mutex

所有 `async_read` / `async_write` / `async_read_some` / `async_write_some` 入口都先
`co_spawn` 到目标 strand，底层 socket 操作期间**不持有任何 mutex**。这样做的收益是：读写路径
上没有锁竞争，且 `close()` 从任意线程投递进来时会被 strand 排队到当前 I/O 之后，不会撕裂。

配套的硬性约定：**每个 socket 操作前都要重新 `stream_->load()` 取快照**。并发
`async_close()` 把 `stream_` 置空只会让在途操作以错误码返回，不会空指针解引用。

### 1.3 配置期与运行期

- **配置期**（`run()` 之前，单线程）：路由注册等无锁结构。见 `httplib::server::router` 的
  线程安全说明——router 内部**没有**任何同步，运行期注册路由是未定义行为。
- **运行期可改**：标量配置一律用 `std::atomic`，指针型配置用
  `std::atomic<std::shared_ptr<T>>` 快照（如 `compress_predicate`、`trusted_proxies`、
  `ssl_context`、`logger`）。请求期读到的是某个一致快照，不会撕裂。

---

## 2. 服务端 `http_server`

### 2.1 两级 strand

```
io_context
└── strand_                    ← server::impl，全局一个
    ├── co_accept() 循环        accept + 会话注册/注销
    ├── async_run()            停机排空
    └── async_stop()           acceptor cancel/close + 遍历 abort
        │
        └── strand（每连接一个）  ← session
            └── conn->run()     读写循环、task 切换
```

`co_accept()`（`lib/server/server_impl.cpp:207`）为**每条连接**新建一个 strand
（`net::make_strand(strand_.get_inner_executor())`），socket 与该 strand 绑定，连接内的所有
协程都跑在上面。这保证一条连接的读写/abort 互相串行，而不同连接之间完全并行。

### 2.2 `sessions_` 只归 `strand_`

`std::unordered_set<std::shared_ptr<session>> sessions_` 仅在 `strand_` 上访问
（`lib/server/server_impl.h:141`）。两处触碰：

- **注册**：`co_accept()` 本协程就在 `strand_` 上，`sessions_.insert(conn)` 是同步的，
  且发生在 `co_spawn(conn->run())` **之前**。这个顺序是停机排空能可靠工作的前提。
- **注销**：`conn->run()` 的完成回调运行在**连接 strand** 上，所以它 `net::dispatch` 回
  `strand_` 再 `sessions_.erase(conn)`。

### 2.3 停机时序

`http_server::stop()` / `async_stop()` 可从任意线程调用，内部 `co_spawn` 到 `strand_`：

1. `acceptor_.cancel()` + `close()` —— 之后不可能再有新会话
2. 遍历 `sessions_` 调 `session::abort()`
3. `co_await stop_event_.wait()` —— `async_run()` 退出时关闭

`async_run()` 侧在 `when_all` 返回后按顺序做：

```
accept 全部结束
  → while (!sessions_.empty()) co_await session_event_.wait();   // 排空在途会话
  → router_.reset();                                             // 此时确无并发读者
  → running_ = false;
```

`router_.reset()` 之所以不需要锁：accept 已停 ⇒ 不会再有新会话；`sessions_` 已空 ⇒ 所有连接
协程都已结束。这是 router 能安全裸奔的前提。

### 2.4 `session::abort()` 不跨线程碰 socket

`session::abort()`（`lib/server/session.cpp:120`）先 `abort_.exchange(true)` 做幂等门闩，
再 `net::post` 回**本连接的 strand** 才执行真正的 `task->abort()`。不能从调用线程直接
`cancel`/`close` 别人的连接对象。

---

## 3. 客户端 `http_client`

### 3.1 一个 strand 包住整个连接

`http_client::impl` 持有一个 `net::strand<net::any_io_executor> strand_`
（`lib/client/client_impl.h:287`），它同时是 `get_executor()` 的返回值。读写模板
（`async_write` / `async_write_some` / `async_read` / `async_read_some`）统一
`co_spawn(strand_, ...)`，内部每次操作前 `stream_.load()` 取快照。

### 3.2 单飞行（single-flight）约束

**同一时刻同一 client 上最多只允许一个请求在途。** eager 请求返回即结束；lazy 请求必须等
响应体读完（或响应对象析构）才结束。

这个约束**没有做运行时防护**——违反会导致同一 socket 的交错读写，是未定义行为。连接池靠
`has_active_session()` 判断复用时机（`lib/client/client_impl.h:83`），手动用 client 时
调用方自负其责。

### 3.3 `stream_mutex_` 保护 lazy 会话配对

`std::recursive_mutex stream_mutex_`（`lib/client/client_impl.h:303`）只保护两件事：
`write_impl_` 与 `read_impl_` 这对 weak_ptr（`lazy_request` / `lazy_response` 的配对关系），
使用点在 `lib/client/client_impl.cpp:112` 与 `:411`。**它不保护 socket I/O**——I/O 靠 strand。

### 3.4 超时计时

`begin_io()` / `end_io()` 按 `timeout_policy_`（`overall` / `step` / `never`）在 strand 上
起停 steady_timer。`overall_timer_active_` 是 atomic，因为判定需要在读侧也能看到。

### 3.5 契约：必须在连接空闲时调用的接口

以下接口做**同步** socket 操作或读 socket 状态，调用时连接必须空闲（无在途请求、无未读完的
lazy 响应），且不得与 `close()` 并发：

| 接口 | 说明 |
| --- | --- |
| `is_alive()` / `async_is_alive()` | 同步 `MSG_PEEK` 探测 TCP 层 FIN/RST，与在途 async I/O 并发操作同一 socket 不允许 |
| `is_open()` | 读 socket open 标志。`stream_` 是原子快照，但探测本身不是线程安全的（Asio shared socket 约定）；最坏返回过期值、不会崩溃 |
| `set_download_rate_limit()` / `set_upload_rate_limit()` | 内部投递到 strand 与 Beast 限速记账串行化，但请求在途时改语义上不安全 |

`close()` / `async_close()` 可从任意线程调用（内部投递到 strand），但**不要在 io 线程的处理
函数里对 `close()` 返回的 future `.get()`**——会死锁，用 `async_close()`。

---

## 4. 连接池 `http_client_pool`

池维护自己的 strand（`ticker(net::make_strand(ex))`，`lib/client/client_pool.cpp:63`），
**每个 client 另有自己的 strand**。池 strand 只管池状态（分桶、计数、waiter 队列、ticker），
不碰 socket。

- **归还连接**（`client_handle` 析构）投递到池 strand。
- **驱逐 / teardown** 在 ticker 的 `on_stop()` / 检查周期回调里执行，都在池 strand 上。
- `on_stop()` 的拆除顺序是先 `move` 出所有待关连接并清空池状态，**再**逐个 `conn->close()`
  （`lib/client/client_pool.cpp:272` 起）——此时已不持有任何池状态，避免关闭过程回调重入池。
- `http_client_pool::stop()` 同步翻转 `is_running_` 并取消 ticker 循环，**不等**实际清理；
  清理在 `on_stop()` 里于池 strand 上异步完成。

---

## 5. WebSocket

### 5.1 服务端 `websocket_conn`

写入用 `util::async_mutex write_mutex_` 串行化（`lib/server/websocket_conn_impl.hpp:66`）。
这是**背压**而非无界队列：并发写会互相等待，而不是排进一个无限增长的队列。

### 5.2 客户端 `ws_client`

自带 strand（`lib/client/ws_client_impl.h:73`），收包/发包/回调都在其上。
`async_abort()` 先 `co_await net::dispatch(strand_)` 再 `stream_.exchange(nullptr)` 并
`shutdown` + `close`（`lib/client/ws_client_impl.cpp:468`）。

### 5.3 `action_queue`

公共 `action_queue` 提供可选的 `max_pending`，用于给待投递动作封顶。

---

## 6. 下载调度器 `download_scheduler`

用 `std::mutex` + pending 队列 + 并发度上限，不走 strand：`dispatch_pending_locked()` 在
调用方持 `mtx_` 时按额度派发（`lib/client/download_scheduler_impl.cpp:351`）。未派发的条目
保持 pending，等下次或收尾时处理。

### 6.1 downloader 落盘 `disk_writer`

`downloader` 的载荷落盘集中在 `disk_writer`（`lib/client/disk_writer.h`），它持有**每个
downloader 一个**的 strand，并独占一次下载的**唯一**输出文件（`open()` 打开一次，`write` /
`write_at` 不再重新打开）。`write`（顺序游标）/ `write_at`（定位偏移）都先 `net::dispatch` 到
该 strand，且文件体内不再 `co_await`，因此同一次下载的多个分片对同一文件的写入彼此串行
（不同 downloader 之间仍并行）。网络协程只负责 `co_await disk_.write_at(...)`，不直接触碰文件。

状态文件 `.dlstate` 只在下载开始前与中止后读写，不与在途分片写入并发，因此走普通同步
`std::fstream`，不经过 strand。

---

## 7. 速查：各组件的并发原语

| 组件 | strand | mutex | atomic | 备注 |
| --- | --- | --- | --- | --- |
| `server::impl` | `strand_`（全局） | — | 超时/压缩/表单/可信代理 | `sessions_` 归 `strand_` |
| `session` | 每连接一个 | — | `abort_` | `abort()` post 回本连接 strand |
| `websocket_conn` | — | `write_mutex_` | — | 背压式串行写 |
| `client::impl` | `strand_` | `stream_mutex_`（仅 lazy 配对） | 超时/limits/SSL/`stream_` | 单飞行为硬约束 |
| `client_pool` | 池 strand | — | 计数 | 不碰 socket |
| `ws_client::impl` | `strand_` | — | `stream_` | 收发同 strand |
| `download_scheduler` | — | `std::mutex` | — | 并发度上限 |
| `disk_writer` | `strand_`（每 downloader 一个） | — | — | 载荷写入在 strand 上串行；单一输出文件 |
| `server::router` | **无** | **无** | — | 仅配置期可写 |

---

## 8. 常见误区

- **「都 `co_spawn` 到 strand 了，那两个协程不会交错」** —— 错。`co_await` 点就会交错。
  跨 `co_await` 的不变量必须用 mutex/atomic 保护。
- **「`stream_` 是原子的，所以随便关」** —— 原子只保证指针读取不撕裂。`exchange(nullptr)`
  与在途 `async_read` 并发是**设计允许**的（后者以错误码返回），但同步 PEEK 之类操作不行。
- **「`stop()` 返回了连接就都关了」** —— 池的 `stop()` 不同步等清理；server 的
  `async_stop()` 会等 `stop_event_`。
- **「router 有锁」** —— 已经没有。运行期注册路由是 UB，见 `router.hpp` 的类级说明。
