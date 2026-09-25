# body 流水线重构设计方案

> 目标：用 Beast `http::buffer_body` 统一做线上读写（每步一个缓冲），解压/压缩逻辑
> 移到 beast 之外自写，string / json / form_data / query_params / file 都变成作用于
> 字节流的**增量 consumer / producer**。删掉 `any_body` 的 variant + 双 holder +
> `compressed_body` 包装这套重型机制。

## 1. 背景与现状问题

当前 `any_body` 机制 (`lib/body/any_body.hpp`)：

- 7 种 body 组成 `std::variant`（`empty/string/json/form/query_params/file/buffer`），
  每种再套一个 `compressed_body<Inner>` 透明编解码。
- `reader_holder` / `writer_holder` 各自包裹 `std::variant<monostate, ...>`，用
  `std::visit` + `match_body` 做双向分发。
- `reader::init` 按 `Content-Type` 现场选择读者；`compressed_body` 在读写过程中同时
  承担解压炸弹限制与编解码。
- 流式解压读取 (`read_some_decompressed`) 借 any_body 塞 `buffer_body` 值 + `pending`
  溢出字段偷鸡（`body_reader.hpp:286-347`）。

问题：三层职责（传输格式 / 业务类型 / 压缩）互相绑定，新增一种 body 需要动模板组合；
`pending` 溢出语义污染了 body 值类型；编译期组合与运行期分发叠加导致臃肿。

## 2. 新架构：三层分块流水线

```
线上字节
  ▼
wire 层   http::parser<IsRequest, http::buffer_body>        // {data,size,more}
  │       每步一个缓冲，满了 need_buffer，换下一块
  ▼
codec 层  body::stream_decoder / stream_encoder = self        // 自己写的解压/压缩胶水
  │       identity 时纯透传；输出累积在自己内部缓冲
  ▼
类型层    读：sink(consumer)   string / json::stream_parser /
                              form_data 状态机 / query_params / file / discard
          写：source(producer) string / json::serializer /
                              form_data 构建器 / file chunk / encoded
```

核心原则：

1. **线上传输永远是一块一块的 `http::buffer_body`**，body 值只是传输 scratch，
   不再承载业务类型。
2. **压缩 / 解压不进 beast body**。读方向：线字节 → `stream_decoder` → sink；
   写方向：source → `stream_encoder` → 线。
3. **业务类型 = 增量消费/生产字节流的对象**（sink/source），不是 beast reader/writer 类型。
4. 业务结果存在独立的 `payload` 里，`as_string()` / `as_json()` 等只是取回结果。

## 3. 组件设计

### 3.1 wire：`http::buffer_body`（Beast）

- value_type：`{ void* data; size_t size; bool more; }`。
- **读**：每次 `read_some` 前设 `body.data/size`（指向暂存缓冲），读走的字节数 =
  `读前 size - 读后 size`；缓冲写满且 body 未完时 beast 返回 `http::error::need_buffer`
  → 换下一块继续。无溢出，因为放不下就直接进下一块。
- **写**：每次 `get()` 前设 `body.data/size/more`，`more=false` 表示该段即结尾。
- 自研 `lib/body/buffer_body.hpp` 删除；`pending` 语义由 `stream_decoder` 的内部输出缓冲替代。

### 3.2 codec：`lib/body/codec.hpp/.cpp`

复用现有 `compress::compressor` 流式接口（`write/buffer/consume/finish`），在此基础上写胶水。

- `class stream_decoder`（读侧，可持久）：
  - 构造：`(http::fields 的 content-encoding, uint64_t body_limit)`
  - `void feed(net::const_buffer raw, error_code& ec)`：喂一块线字节。
  - `size_t drain(net::mutable_buffer dst, error_code& ec)`：把已解输出的字节拷进
    caller 缓冲，剩余留在内部 `std::string out_`（返回时先取走存量）。
  - `void flush(error_code& ec)`：解压流收尾（`compressor->finish` + 排空）。
  - 解压炸弹：输出字节记账，超过 `body_limit` 返回 `http::error::body_limit`
    （从 `compressed_body::reader` 迁移，含"content-length 已接近上限则提前拒绝"）。
  - identity / 未知编码：不进 compressor，`feed` 直接进 `out_`（保留字节计数以限流）。
- `class stream_encoder`（写侧）：对称，`feed(plain)` / `drain(compressed)` / `flush`。
- 一次性辅助：`std::string decode(bytes, encoding, limit, ec)`、
  `std::string encode(bytes, encoding, ec)`（内部一个 decoder 读到 end 再拼接）。

### 3.3 读侧 sink：`lib/body/sink.hpp`（增量消费者）

统一接口：

```cpp
class sink {
public:
    using result_t = ...;              // 该 sink 最终产物
    virtual void put(net::const_buffer chunk, error_code& ec) = 0;   // 逐块喂
    virtual void finish(error_code& ec) = 0;                          // 输入结束
    virtual result_t release() = 0;                                    // 取结果
};
```

具体实现：

| sink | 产物 | 说明 |
|---|---|---|
| `string_sink` | `std::string` | chunk 直接 append（原 `string_body::reader` 的逻辑） |
| `json_sink` | `boost::json::value` | 喂 `boost::json::stream_parser`（本身增量）；沿用 10 MiB 上限与预分配 resource 的思路 |
| `query_params_sink` | `httplib::query_params` | 累积原始字节，`finish` 时 urldecode |
| `form_data_sink` | `httplib::form_data` | 复用现 multipart 状态机；需 `Content-Type`(boundary) + `form_data::param` |
| `file_sink` | `void` | 复用 `file_body::value_type`（open/write），chunk 直接写盘 |
| `discard_sink` | `void` | 空 body / 无需消费时逐块丢弃 |

`form_data` 状态机从 `lib/body/form_data_body.hpp/.cpp` 抽成独立的增量类
`multipart_parser`（含 boundary 半拆分 / eof / 路径穿越检查），`form_data_sink` 包一层。

**分发**：`body_reader` 里一个普通函数，按 `Content-Type` 选 sink（原 `any_body::reader::init`
的映射）；`body_setup` 从"预置 variant 值"改为"配置 sink"（如 form_data 的 param、
file 的保存路径）。

### 3.4 写侧 source：`lib/body/source.hpp`（增量生产者）

统一接口：

```cpp
class source {
public:
    using chunk_t = boost::optional<std::pair<net::const_buffer, bool /*more*/>>;
    virtual chunk_t next(error_code& ec) = 0;   // 逐块产出；none 表示取尽
};
```

| source | 来源 | 说明 |
|---|---|---|
| `string_source` | `std::string` | 一块发完，`more=false` |
| `json_source` | `boost::json::value` | 包 `boost::json::serializer`（增量吐 chunk） |
| `form_data_source` | `std::vector<httplib::form_data::field>` | 复用现 multipart 构建器（boundary 头 / 字段 / 文件 8 KiB 流式） |
| `file_source` | `fs::path` + ranges | 复用现 `file_body::writer` 的 16 KiB chunk 读取与 multipart/byteranges 组装 |
| `encoded_source` | `source*` + encoding | 包 `stream_encoder`：plain chunk → 压缩 chunk |
| `empty_source` | — | `next` 直接 none |

- `file_body` 的 value_type 收窄为"文件 + ranges + 内容类型"的**只写数据**，
  不再参与 variant。
- 响应/请求统一由"source 链"产出字节 → 再落到 `body().data/size/more`。

### 3.5 结果容器：`lib/body/payload.hpp/.cpp`（取代 `body_state` + `access`）

```cpp
class payload {
    enum class kind { none, empty, string, json, query_params, form_data, file };
    kind kind_ = kind::none;
    std::string text_;                    // string/query_params 原始(text_)、form_data 的备用
    boost::json::value json_;
    httplib::query_params query_params_;
    httplib::form_data form_data_;
public:
    bool has() const;
    kind type() const;
    bool is_empty() const;
    std::string const&   as_string() const;       // == text_
    boost::json::value const& as_json() const;
    httplib::query_params const& as_query_params() const;
    httplib::form_data const&    as_form_data() const;
    void assign(kind, 各产物&&);                    // sink.release 后写入
    // take<T>() 迁移语义保留
};
```

- `is_*` 语义变化：不再看"variant 里存的是哪一型"，而是看**读取时按 Content-Type 选
  的 sink 种类**（存进 `kind_`）。`as_*` 仍是"取物化结果"。
- JSON/query_params 的解析发生在 sink 内，错误照旧在读阶段上浮为 error_code，
  不在 `as_json()` 里抛。

## 4. 读流水线

### 4.1 `read_body`（整体物化）

`lib/body/body_reader.hpp` 改：

```
any_parser_t(http::parser<IsRequest, any_body>)   ──删除──
体读改用 raw_parser_t = http::parser<IsRequest, http::buffer_body>

read_body(setup, ec):
  按 Content-Type 建 sink（setup 先行配置 sink）
  循环直到 parser.is_done():
    body.data/size ← staging_(64 KiB 复用缓冲)
    co_await read_some(parser, ec)                 // need_buffer 视为正常
    consumed = staging_.size() - body.size
    if consumed: decoder.feed(const_buffer(staging_, consumed))
                decoder.drain_to(sink)             // 反复排空
  decoder.flush() → sink.finish()
  payload.assign(sink.release())
```

- `decompressed_limit`（原 `body().decompressed_limit`，body_reader.hpp:194）→ 由
  `body_limit_` 传进 `stream_decoder` 构造。
- `store_body` → `payload.assign(...)`；`body_state` 引用点全部换成 `payload`。

### 4.2 `read_some_raw`（原样保留）

现实现就是 `http::buffer_body` parser + caller 缓冲（body_reader.hpp:234-282），
与新车同构，基本不动（只把 raw_parser_t 的别名/类型对齐）。

### 4.3 `read_some_decompressed`（去掉 pending 偷鸡）

body_reader 持有持久 `stream_decoder`：

```
循环:
  if decoder 内部 out_ 有存量: 拷到 caller buf, 返回
  else if parser.is_done():    decoder.flush(); …… 返回 0
  else:                        staging → feed → drain into caller buf
```

`is_body_done()`（body_reader.hpp:77-113）改为：`parser.is_done()` 且 decoder
内部缓冲已排空。不再引用 `any_body`/`pending`。

## 5. 写流水线

统一套路（session 响应 / client 请求 / stream_writer 共用）：

```
resp/req body 类型 = http::buffer_body
1. 业务数据 → 构造 source（string/json/form_data/file/empty）
2. 要压缩 → 包 encoded_source
3. 序列化循环:
   建 http::response_serializer<http::buffer_body>(msg)
   while (!sr.is_done()):
      若上一块 more（或首次）: chunk = source.next(ec); 若无 → 结束
        msg.body().data = chunk.first.data(); size = chunk.first.size(); more = chunk.second
      co_await write_some(sr, ec)      // need_buffer 正常
```

### 5.1 服务器响应（`lib/server/session.cpp`）

- 压缩决策（session.cpp:429-444）不动：accept-encoding 协商 + `should_compress_content_type`。
- 统一规则：**命中压缩 → chunked + `encoded_source`**（文件不必整块进内存；
  与现状一致，因为压缩后无法预知长度）。**不压缩 → 尽量设 `Content-Length`，source
  一块发完**（string/json/form_data/单 range 文件）；multipart/byteranges 多段文件
  长度未知 → chunked，与现状一致。
- `http::response_serializer<body::any_body>`（session.cpp:451）→
  `http::response_serializer<http::buffer_body>`。
- HEAD：仍 `reset_content()` + 写头即可（`empty_source`）。

### 5.2 服务器流式写（`lib/server/stream_writer_impl.hpp`）

- 直连流式分支（现 any_body + buffer_body 值舞步，行 60-64、101-109）与 relay 分支
  （Beast `http::buffer_body`，行 54-56、111-114）**统一**：都走 `http::buffer_body`
  序列化器，`write_body(data, more)` 直接填 `body.data/size/more`。删 any_body。

### 5.3 客户端请求（`lib/client/client_impl.cpp` + `lib/client/request.cpp`）

- `request::impl : http::request<http::buffer_body>`。
- `set_body(string/json/form_data/query_params)`、`set_file_body`：业务对象 → source
  （json 走 `json_source`，form_data 走 `form_data_source`，文件走 `file_source`）。
- 序列化（client_impl.cpp:119）换成 `http::request_serializer<http::buffer_body>`。
- `prepare_request`（client_impl.cpp:305-346）简化：
  - 不支持的 Content-Encoding 仍删头告警（保留）；
  - 空 body 显式 `Content-Length: 0`（保留）；
  - 显式设置了 transform encoding 的请求 → chunked + `encoded_source`（原注释逻辑
    保留，实现从"any_body writer 隐式压缩"改为 source 链显式压缩）。

## 6. 文件改动清单

**新建**
- `lib/body/codec.h/.cpp`：stream_decoder / stream_encoder / 一次性 decode·encode
- `lib/body/source.hpp`：source 接口 + string/json/form_data/empty/encoded source
- `lib/body/file_source.hpp/.cpp`：文件（含 ranges/multipart/byteranges）chunk 读取
- `lib/body/sink.hpp`：sink 接口 + string/json/query_params/discard sink
- `lib/body/multipart_parser.hpp/.cpp`：从 form_data_body 抽出的增量 multipart 解析器
- `lib/body/body_state.hpp`：共用体（`std::variant`）结果容器（none/empty/string/json/
  query_params/form_data/file）

**删除**
- `lib/body/any_body.hpp`
- `lib/body/compressed_body.hpp`
- `lib/body/buffer_body.hpp`（自研，被 Beast `http::buffer_body` 取代）
- `lib/body/empty_body.hpp`、`string_body.hpp`、`json_body.hpp`、`query_params_body.hpp`、
  `form_data_body.hpp`（`file_body.hpp` 收窄为只写来源）

**修改（涉及 any_body / body_state 的点）**
- `lib/body/body_reader.hpp`：体读改 buffer parser + decoder + sink；持久 decoder；
  `is_body_done` 去掉 pending；**承担全部读取任务**（read_some_raw/decompressed、
  read_body 自动/强制 sink、read_string/read_json/read_query_params/read_form_data/
  read_to_file）
- `lib/body/read.hpp`：删除（free 函数并入 body_reader 成员）
- `lib/client/request_impl.h`、`lib/client/response_impl.h`、`lib/server/request_impl.hpp`、
  `lib/server/response_impl.hpp`：message body 类型 → `http::buffer_body`；
  持有 `body_state` / `source`
- `lib/client/request.cpp`、`lib/client/response.cpp`、`lib/server/request.cpp`：
  `as_*` 走 body_state；`read_*` 走 body_reader；set_body* 走 source
- `lib/server/session.cpp`、`lib/server/stream_writer_impl.hpp`：写循环 + 压缩决策
- `lib/client/client_impl.cpp`：序列化器类型 + prepare_request
- 测试：`tests/body_reader_test.cpp`、`tests/compressor_test.cpp` 中直接构造
  `any_body::reader` 的点改为对着 sink/codec 测

## 7. 公共 API 语义变化（对齐点）

| API | 旧 | 新 |
|---|---|---|
| `as_string()` | 返回 variant 中 string | 返回 payload.text_（string sink 结果） |
| `as_json()` | 返回 variant 中的 json::value | 返回 json sink 解析结果（读时即解析） |
| `is_json()/is_string()/...` | variant 持有类型 | 读取时按 Content-Type 选的 sink 种类 |
| `client::request::as_json()` | 返回用户 set 的原值 | 变为从字节重解析（注意） |
| `read_to_file(path)` | body_setup 预置 file_body 值 | file_sink 打开文件即可 |

## 8. 行为等价性核对表（改完逐条验）

- [ ] 读：string/json/form_data/query_params/空 body 物化、Content-Type 分发与现状一致
- [ ] 读：`body_limit`（解压炸弹）在 eager 与 streaming 两个路径都生效
- [ ] 读：`read_some_raw` / `read_some_decompressed` 字节一致（含 identity 透传）
- [ ] 写：服务器 string / json / form_data / 单文件 / ranges / multipart byteranges /
      空 / 重定向 / HEAD 一致
- [ ] 写：压缩协商（gzip/deflate/zstd/br）+ chunked 行为一致；未压缩响应可带 Content-Length
- [ ] 写：`stream_writer`（SSE 等）字节一致；proxy relay 不受影响
- [ ] 客户端：set_body×4 / set_file_body / 手动 Content-Encoding / 默认 Accept-Encoding 一致
- [ ] sse/ndjson 读取路径（基于 read_some_decompressed）一致
- [ ] 测试构造点迁移后全绿：`ctest --test-dir build -C Debug`

## 9. 实施步骤（建议顺序）

1. **M1**：新增 `codec` / `sink` / `source` / `multipart_parser`，独立于现有代码可编译，
   补单测（对着压缩字节流喂 decoder→sink 断言产物）。
2. **M2**：重写 `body_reader` 读流水线（buffer_body + decoder + sink），删 any_body/
   compressed_body 读侧；server/client 读侧转 payload。
3. **M3**：重写写流水线（session / stream_writer / client），删写侧旧 body；压缩走
   stream_encoder。
4. **M4**：删自研 buffer_body / empty_body / string_body / json_body / query_params_body /
   form_data_body，清文档与测试残留，跑全量 ctest 做等价核对。