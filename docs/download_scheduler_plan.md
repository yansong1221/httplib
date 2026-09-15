# download_scheduler 新版实现计划

## 一、背景与目标

当前 `downloader` 是单文件下载器，一次处理一个 URL。

目标新增 `download_scheduler`，负责管理多个下载任务，并提供：

- 批量提交多个 URL 下载任务
- 最大并发下载数量控制
- per-task cancel / pause / resume
- per-task 状态与进度回调
- 任务状态查询
- `async_run()` 协程驱动
- 动态 `add()`：scheduler 运行期间允许加入任务
- `async_wait_any()` / `async_wait_one()` 等等待接口
- 与现有 Boost.Asio coroutine / `http_client_pool` 架构保持一致

核心设计原则：

> scheduler 负责调度和任务生命周期；downloader 负责单个下载；http_client_pool 负责连接复用。

---

## 二、总体架构

采用：

```text
                    download_scheduler
                           |
                    scheduler strand
                           |
             +-------------+-------------+
             |             |             |
          pending        running      completed
             |             |             |
             +-------------+-------------+
                           |
                    scheduler event
                           |
                     scheduler loop
                           |
                 +---------+---------+
                 |                   |
          start pending        reap completed
```

## 设计原则

### 2.1 scheduler 内部状态统一由 strand 串行化

推荐使用：

```cpp
net::strand<net::any_io_executor> strand_;
```

scheduler 内部状态：

- `tasks_`
- `pending_queue_`
- `running_count_`
- `completed_queue_`
- `config_`

全部只允许在 `strand_` 上访问。

因此 scheduler 不使用：

```cpp
std::mutex
std::atomic
```

来保护内部状态。

---

### 2.2 所有外部 API 都通过 strand 修改状态

例如：

```cpp
void cancel(task_id id)
{
    net::post(strand_,
        [this, id] {
            do_cancel(id);
        });
}
```

需要同步查询时，可以通过 coroutine / promise 等方式在 strand 上获取结果。

原则：

> 不允许外部线程直接修改 scheduler 内部状态。

---

### 2.3 不使用 detached coroutine 捕获裸 `this`

禁止：

```cpp
co_spawn(
    executor_,
    [this] { ... },
    net::detached);
```

scheduler 必须拥有明确的运行生命周期。

推荐：

```text
scheduler
    |
    +-- scheduler coroutine
    |
    +-- task coroutine
```

析构或显式 stop 时：

```text
stop
 ↓
停止接收新任务
 ↓
cancel scheduler
 ↓
cancel 所有 task
 ↓
等待 task coroutine 退出
 ↓
scheduler coroutine 退出
 ↓
释放 impl
```

---

## 三、downloader 前置修改

### 3.1 修复 multi-segment `when_all` bug

文件：

```text
lib/client/downloader_impl.cpp
```

当前问题：

```cpp
std::vector<boost::system::error_code> errors(seg_count);

co_await util::when_all(std::move(ops));
```

`when_all()` 的返回结果没有保存，因此 segment 错误不会被检查。

修改：

```cpp
auto results = co_await util::when_all(std::move(ops));

for (auto const& ec : results)
{
    if (ec)
    {
        // cancel remaining segments if necessary
        // cleanup temporary files
        co_return ec;
    }
}
```

需要进一步明确：

- 一个 segment 失败后是否取消其他 segment
- 是否等待其他 segment 完成
- 临时文件清理策略
- segment retry 策略
- 最终 error 选择规则

---

## 四、downloader pause/resume 重新设计

### 4.1 状态

```cpp
enum class state
{
    idle,
    connecting,
    downloading,
    paused,
    merging,
    completed,
    failed,
    cancelled
};
```

---

### 4.2 不使用 `post()` polling

禁止：

```cpp
while (paused_)
{
    co_await net::post(executor_, net::use_awaitable);
}
```

该实现会不断重新调度 coroutine，形成 event-loop spin。

必须使用真正的等待事件。

---

### 4.3 pause event

downloader 内部增加可等待的 pause/resume event。

逻辑：

```text
running
   |
 pause()
   |
 paused
   |
   +------ wait resume/cancel
              |
        +-----+-----+
        |           |
      resume      cancel
        |           |
     running     cancelled
```

pause 时：

```cpp
paused_ = true;
```

下载 coroutine：

```cpp
while (paused_)
{
    set_state(state::paused);

    auto ec = co_await pause_event_.wait();

    if (cancelled_)
        co_return net::error::operation_aborted;

    if (ec)
        co_return ec;
}
```

---

### 4.4 resume 必须唤醒 coroutine

```cpp
void resume()
{
    paused_ = false;
    pause_event_.notify_all();
}
```

不能只修改一个 atomic flag。

---

### 4.5 cancel 必须唤醒 paused coroutine

```cpp
void cancel()
{
    cancelled_ = true;

    pause_event_.notify_all();

    // 同时取消当前 HTTP operation
    cancellation_signal_.emit(...);
}
```

要求：

> paused 状态下调用 cancel，必须能够立即结束任务。

---

### 4.6 pause 语义

pause 是 cooperative pause。

如果当前正在：

```cpp
co_await resp.read_some_raw(...);
```

调用 pause 后不会强制中断当前 read。

当前 read 完成后进入 paused。

因此 pause 不保证"调用瞬间"停止网络读取。

---

## 五、download_scheduler 数据模型

### 5.1 task_entry

推荐：

```cpp
struct task_entry
{
    task_id id;

    std::string url;
    fs::path save_path;

    task_options options;

    std::unique_ptr<downloader> dl;

    task_status status;

    bool pause_requested = false;
    bool cancel_requested = false;

    bool started = false;
};
```

---

### 5.2 task 状态

scheduler 内部需要明确区分：

```text
pending
running
terminal
```

downloader 状态继续负责：

```text
idle
connecting
downloading
paused
merging
completed
failed
cancelled
```

建议：

```cpp
bool is_terminal(downloader::state state)
{
    return state == downloader::state::completed ||
           state == downloader::state::failed ||
           state == downloader::state::cancelled;
}
```

---

## 六、并发限制

配置：

```cpp
struct scheduler_config
{
    std::size_t max_concurrent = 8;
};
```

定义：

> `max_concurrent` 限制 running downloader 数量。

建议 paused task：

> 仍然占用 concurrency slot。

原因：

- HTTP connection 仍然可能保持
- downloader 仍然属于 running task
- 防止大量 pause 后无限启动新任务

如果未来需要释放 slot，可以新增：

```cpp
pause_mode::hold_connection
pause_mode::release_connection
```

当前版本不实现。

---

## 七、任务生命周期

完整状态：

```text
                 add
                  |
                  v
               pending
              /       \
           pause      cancel
            |           |
            v           v
       paused-pending cancelled
            |
          resume
            |
            v
          pending
            |
       scheduler dispatch
            |
            v
          running
        /    |     \
    pause  cancel  complete
      |       |       |
      v       v       v
   paused cancelled terminal
      |
    resume
      |
      v
   running
```

---

## 八、pending task cancel

这是 scheduler 自己必须处理的。

例如：

```cpp
auto id = scheduler.add(...);
scheduler.cancel(id);
```

如果任务还没有启动：

```text
pending
  |
 cancel
  |
cancelled
```

不得再进入 downloader。

`pending_queue_` 可以采用：

### 方案 A：lazy deletion

队列仍保留 id：

```cpp
pending_queue_.push_back(id);
```

dispatch 时检查：

```cpp
if (entry->cancel_requested)
    continue;
```

简单可靠，推荐。

---

## 九、pending task pause

如果：

```cpp
auto id = add(...);
pause(id);
```

建议：

```text
pending
  |
 pause
  |
paused-pending
```

scheduler 不启动该任务。

resume：

```text
paused-pending
      |
    resume
      |
   pending
```

这样不会浪费 downloader/HTTP connection。

---

## 十、scheduler event

scheduler 需要真正的 event，而不是 timer polling。

事件至少包括：

```text
task_added
task_completed
task_cancelled
task_resumed
config_changed
scheduler_stop
```

核心循环：

```cpp
net::awaitable<void> run_loop()
{
    for (;;)
    {
        dispatch_pending();

        if (should_exit())
            co_return;

        co_await event_.wait();
    }
}

```

---

## 十一、事件触发规则

## add

```text
add()
 ↓
post strand
 ↓
pending_queue_.push_back(id)
 ↓
event.signal()
```

---

## task complete

```text
downloader coroutine
 ↓
task finished
 ↓
strand
 ↓
running_count_--
completed_queue_.push_back(id)
 ↓
event.signal()
```

---

## resume

```text
resume()
 ↓
strand
 ↓
pause_requested = false
 ↓
event.signal()
```

---

## config changed

```text
set_scheduler_config()
 ↓
strand
 ↓
config_ = new_config
 ↓
event.signal()
```

---

## 十二、scheduler coroutine 生命周期

不使用 detached coroutine 的裸 `this` 模式。

推荐：

```text
download_scheduler
      |
      +-- shared impl/state
              |
              +-- scheduler coroutine
              |
              +-- task coroutine
```

所有 coroutine 对 state 的访问必须保证 state 存活。

析构流程：

```text
~download_scheduler()
        |
        v
request_stop()
        |
        v
cancel scheduler event
        |
        v
cancel all downloader
        |
        v
等待 scheduler/task coroutine 完成
        |
        v
destroy state
```

如果当前 API 不方便让析构等待 coroutine，则应提供：

```cpp
net::awaitable<void> async_shutdown();
```

由调用方在生命周期结束前显式等待。

---

## 十三、建议增加 `async_shutdown()`

```cpp
net::awaitable<void> async_shutdown();
```

语义：

```text
停止接收新任务
      ↓
取消 pending
      ↓
取消 running
      ↓
唤醒所有 waiters
      ↓
等待所有 task coroutine
      ↓
scheduler coroutine 退出
```

这比单纯依赖 destructor 更安全。

---

## 十四、download_scheduler API

推荐：

```cpp
class HTTPLIB_API download_scheduler
{
  public:
    using task_id = std::uint64_t;

    struct scheduler_config
    {
        std::size_t max_concurrent = 8;
    };

    struct task_options
    {
        downloader::config dl_config;
        http::fields headers;
    };

    struct task_status
    {
        task_id id = 0;

        std::string url;
        fs::path save_path;

        downloader::state state =
            downloader::state::idle;

        std::uint64_t total_bytes = 0;
        std::uint64_t downloaded_bytes = 0;
        std::uint64_t speed_bytes_per_sec = 0;

        std::string message;
    };

    using progress_callback =
        std::function<void(task_status const&)>;

    using state_callback =
        std::function<void(task_status const&)>;

  public:
    download_scheduler(
        net::any_io_executor ex,
        std::shared_ptr<http_client_pool> pool,
        scheduler_config cfg = {});

    ~download_scheduler();

    download_scheduler(download_scheduler const&) = delete;
    download_scheduler& operator=(
        download_scheduler const&) = delete;

    download_scheduler(download_scheduler&&) = delete;
    download_scheduler& operator=(
        download_scheduler&&) = delete;

    task_id add(
        std::string_view url,
        fs::path const& save_path,
        task_options opts = {});

    void cancel(task_id id);
    void cancel_all();

    void pause(task_id id);
    void resume(task_id id);

    task_status get_status(task_id id) const;
    std::vector<task_status> get_all_status() const;

    std::size_t active_count() const;
    std::size_t pending_count() const;
    std::size_t total_count() const;

    void set_progress_callback(progress_callback cb);
    void set_state_callback(state_callback cb);

    void set_scheduler_config(
        scheduler_config const& cfg);

    scheduler_config get_scheduler_config() const;

    net::awaitable<void> async_run();

    net::awaitable<void> async_run_all(
        std::vector<
            std::tuple<
                std::string,
                fs::path,
                task_options>>& tasks);

    net::awaitable<task_status> async_wait_any();

    net::awaitable<task_status> async_wait_one(task_id id);

    net::awaitable<void> async_shutdown();
};
```

---

## 十五、查询 API 的线程安全

不要返回：

```cpp
scheduler_config const&
```

应该：

```cpp
scheduler_config get_scheduler_config() const;
```

返回 copy。

状态查询也应该返回 snapshot：

```cpp
task_status get_status(task_id id) const;
```

不能暴露内部 `task_entry`。

---

## 十六、callback 设计

callback 永远不能在 scheduler 内部锁/临界区执行。

流程：

```text
update state
     |
copy callback
     |
copy task_status
     |
leave scheduler state
     |
invoke callback
```

允许 callback 中重新调用：

```cpp
scheduler.add(...)
scheduler.cancel(...)
scheduler.pause(...)
scheduler.resume(...)
```

这些操作重新 post 到 scheduler strand。

---

### callback 异常

callback 异常不能导致 scheduler coroutine 退出。

建议：

```cpp
try
{
    cb(status);
}
catch (...)
{
    // log callback exception
}
```

---

## 十七、active_count 定义

推荐：

```text
active_count =
    connecting
  + downloading
  + paused
  + merging
```

paused task 仍然占用 slot。

terminal 状态不计入。

pending 不计入。

---

## 十八、配置动态修改

例如：

```text
max_concurrent = 8
active = 8
```

修改：

```text
max_concurrent = 2
```

不暂停已有任务。

结果：

```text
active = 8
limit = 2
```

只是不再启动新任务。

当：

```text
active < 2
```

以后才继续启动。

反过来：

```text
2 -> 8
```

需要：

```text
config_changed
    ↓
event.signal()
    ↓
scheduler 立即 dispatch
```

---

## 十九、async_wait_any

定义为：

> 等待下一个进入 terminal 状态的任务。

完成事件进入：

```cpp
completed_queue_
```

调用：

```cpp
co_await async_wait_any();
```

消费一个：

```text
completed_queue_.front()
```

并返回 snapshot。

一个完成任务只消费一次。

---

## 二十、async_wait_one

定义：

> 等待指定 task 第一次进入 terminal 状态。

如果 task 已经完成：

```cpp
async_wait_one(id)
```

立即返回。

如果不存在：

建议返回：

```cpp
boost::system::errc::no_such_file_or_directory
```

或项目统一定义的 task-not-found error。

---

## 二十一、禁止 timer polling

不要采用：

```cpp
steady_timer
 ↓
async_wait
 ↓
cancel
 ↓
重新创建 timer
```

作为 scheduler 主事件循环。

timer 只应该用于：

- timeout
- deadline
- retry delay

scheduler 的：

- add
- complete
- cancel
- resume
- shutdown

全部使用 event。

---

## 二十二、任务完成处理

task coroutine：

```cpp
auto ec = co_await entry->dl->async_download(...);
```

结束后：

```text
task coroutine
      |
      v
post/dispatch scheduler strand
      |
      +-- 更新 task state
      +-- running_count_--
      +-- completed_queue_.push_back(id)
      +-- signal scheduler event
```

不能在 downloader coroutine 中直接无保护修改 scheduler 状态。

---

## 二十三、task coroutine 与 scheduler 解耦

建议 downloader 完成后只产生：

```cpp
task_completion
{
    task_id id;
    error_code ec;
};
```

scheduler 收到后负责：

```text
完成状态
active_count
completed_queue
callback
下一任务 dispatch
```

downloader 不应该知道 scheduler 的内部实现。

---

## 二十四、测试计划

### 基础

- 3 个 URL 并发下载
- `max_concurrent=2`
- 5 个任务峰值并发 <= 2
- 单任务 cancel
- cancel_all
- pause/resume
- progress callback URL/id 正确
- state callback URL/id 正确
- scheduler 运行期间 add
- get_status
- get_all_status
- empty scheduler 立即结束

---

### pending

增加：

- cancel pending task
- pause pending task
- resume paused-pending task
- cancel paused-pending task

---

### 生命周期

必须测试：

- downloading 时 scheduler shutdown
- paused 时 scheduler shutdown
- pending 时 scheduler shutdown
- scheduler 析构时存在 running task
- scheduler 析构时存在 paused task
- shutdown 后不能 add
- shutdown 后 callback 不再访问已经销毁对象

---

### 并发

多线程同时：

```text
add
cancel
pause
resume
get_status
set_config
```

验证：

- 无 data race
- 无 deadlock
- 无 crash
- 无 lost wakeup
- 无 task 重复启动
- active_count 始终正确

---

### callback

测试：

- callback 中 `add`
- callback 中 `cancel`
- callback 中 `pause`
- callback 中 `resume`
- callback 抛异常
- callback 触发 scheduler shutdown

---

### downloader

测试：

- single download pause/resume
- multi-segment pause/resume
- paused cancel
- download read 过程中 pause
- pause 后 resume
- segment failure
- segment cleanup
- server 500
- retry
- invalid URL

---

## 二十五、实现顺序

```text
Phase 0
    修复 multi-segment when_all
        ↓
Phase 1
    downloader pause/resume event
        ↓
Phase 2
    downloader 生命周期/cancel 处理
        ↓
Phase 3
    scheduler task/event/state 基础结构
        ↓
Phase 4
    strand 调度器
        ↓
Phase 5
    pending/running/completed
        ↓
Phase 6
    cancel/pause/resume
        ↓
Phase 7
    async_wait_any/one
        ↓
Phase 8
    shutdown/lifetime
        ↓
Phase 9
    callback
        ↓
Phase 10
    动态 config
        ↓
Phase 11
    CMake
        ↓
Phase 12
    单元测试
        ↓
Phase 13
    压力测试 / ThreadSanitizer / ASan
```

---

## 二十六、关键设计决策

### 为什么一个任务一个 downloader？

因为 downloader 内部状态属于单任务：

```text
state
segments
progress
response
temporary files
```

每个任务独立 downloader 可以避免状态串扰。

HTTP connection 仍通过：

```cpp
std::shared_ptr<http_client_pool>
```

共享。

---

### 为什么 scheduler 使用 strand？

scheduler 的状态天然属于一个状态机。

使用 strand 后：

```text
tasks_
pending_
running_
completed_
config_
```

全部串行访问。

避免：

```text
mutex + atomic + callback + coroutine
```

组合造成复杂锁关系。

---

### 为什么不用 timer 做 scheduler event？

timer 是时间语义。

scheduler 需要的是：

```text
event happened
```

因此使用 event 更直接：

```text
add      -> signal
complete -> signal
resume   -> signal
stop     -> signal
```

timer 只处理真正的 timeout。

---

### 为什么 pause 不释放 concurrency slot？

当前 pause 不关闭 HTTP connection。

因此任务仍然持有：

- downloader
- response
- connection

所以继续占用 scheduler slot 更符合实际资源使用情况。

---

## 二十七、最终验收标准

实现完成后必须满足：

### 调度

- [ ] max_concurrent 永不超限
- [ ] pending task 不会提前启动
- [ ] pending cancel 不会启动
- [ ] pending pause 不会启动
- [ ] resume 后能够重新调度
- [ ] 动态 add 能立即唤醒 scheduler

### pause

- [ ] 不使用 post polling
- [ ] pause 后 coroutine 真正等待
- [ ] resume 能唤醒
- [ ] cancel 能唤醒 paused coroutine
- [ ] paused task 状态正确

### 生命周期

- [ ] 无 detached coroutine 捕获裸 this
- [ ] shutdown 后所有 task coroutine 退出
- [ ] scheduler 析构不存在 UAF
- [ ] paused task 析构可以退出
- [ ] running task 析构可以退出

### 并发

- [ ] scheduler 内部状态全部 strand 串行化
- [ ] 不需要 mutex 保护内部状态
- [ ] callback 不在 scheduler 状态修改临界区执行
- [ ] callback 可重入 scheduler API
- [ ] 无 lost wakeup
- [ ] 无 deadlock

### 等待

- [ ] async_wait_any 只消费一个 completion
- [ ] async_wait_one 已完成任务立即返回
- [ ] task 不存在时有明确错误
- [ ] 不依赖 timer polling

### 测试

- [ ] 基础下载
- [ ] 并发限制
- [ ] cancel
- [ ] pause/resume
- [ ] pending cancel
- [ ] pending pause
- [ ] shutdown
- [ ] callback 重入
- [ ] callback exception
- [ ] 多线程压力测试
- [ ] ASan
- [ ] ThreadSanitizer（如果项目环境允许）