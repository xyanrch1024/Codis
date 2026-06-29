# Codis C++ C/S 架构设计

## 总体拓扑

```
┌──────────────┐  fire-and-forget (REST)  ┌─────────────────────────────────┐
│  codis        │ ◄─────────────────────► │  codis-server                   │
│  (CLI/TUI)   │  SSE long-lived stream   │  (后台守护进程)                  │
│              │                          │                                 │
│  send()      │  POST /api/v1/acp        │  ├─ SessionState (per session)  │
│  connect()   │  ──────────────────────► │  │   conns: map<conn_id, queue> │
│  后台SSE线程  │  GET /api/v1/acp/stream   │  │   processing: atomic<bool>   │
│  send_async()│ ◄══ SSE keep-alive ═════ │  ├─ ProviderRegistry           │
│              │                          │  ├─ ToolRegistry (6)           │
│  交互命令:    │                          │  ├─ SystemContext (6)          │
│  /sessions   │                          │  ├─ SessionStore (SQLite)      │
│  /session id │                          │  └─ Logger                     │
│  /clear      │                          │                                 │
│  /clearsessions                         │                                 │
└──────────────┘                          └─────────────────────────────────┘
```

## 通信架构 (v0.10.0)

### SessionState — 每 session 的直接队列广播

EventBus 已从 SSE 路径移除，改为 SessionState 管理每个 session 下的所有连接队列：

```
                     SessionState (per session_id)
                 ┌──────────────────────────────────────┐
                 │  conns: map<conn_id, SseFrameQueue>  │
                 │  processing: atomic<bool>             │
                 │  mutex                                │
                 │                                      │
LLM Worker ─────┤  broadcast(frame, conn_id)             │
  (run_acp_    │                                      │
   loop_       │  conn_id 非空 → push target queue      │──→ SSE A
   broadcast)  │  conn_id 为空 → push all queues        │──→ SSE B
                 │                                      │
                 └──────────────────────────────────────┘
```

- 每个 SSE 连接分配唯一 `conn_id`
- ACP 请求可携带 `conn_id` 精确路由 LLM 输出
- `conn_id` 为空时广播到该 session 的所有连接（向后兼容）
- 断开时自动清理：`sink.write()` 失败触发 `cleanup_connection()`

### SSE 长连接 (keepalive)

```
Client                                    Server
  │                                         │
  │  GET /api/v1/acp/stream/{id}           │  allocate conn_id
  │    ?keepalive=1                        │  push connected frame
  │                                        │
  │  ←── {"type":"connected","conn_id":"x"}  │
  │  ←── history frames ─────────────────  │
  │                                        │
  │  POST /api/v1/acp                      │  fire-and-forget → LLM
  │  {"session_id","messages","conn_id"}   │  → 202 Accepted
  │                                        │  → run_acp_loop_broadcast()
  │  ←── assistant frames ──────────────  │  → broadcast to conns[conn_id]
  │  ←── tool_call ──────────────────────  │
  │  ←── tool_result ────────────────────  │
  │  ←── {"type":"done"} ────────────────  │
  │                                        │  keepalive: 不关闭连接
  │  POST /api/v1/acp                      │  下一轮 ACP
  │  ←── assistant frames ──────────────  │
  │  ←── done ───────────────────────────  │
  │                                        │
  │  disconnect                             │  sink.write() fail
  │                                        │  → cleanup_connection()
```

| 参数 | 效果 |
|------|------|
| `?keepalive=1` | done 后不关闭连接，持续等下一轮 |
| 无参数 | done 后关闭连接（`send()` 使用） |

### 端点

| 端点 | 方法 | 连接 | 说明 |
|------|------|------|------|
| `/api/v1/acp/stream/{id}` | GET | 长连接 | SSE stream，分配 conn_id |
| `/api/v1/acp` | POST | 短连接 | fire-and-forget，传 conn_id 精准路由 |
| `/api/v1/chat` | POST | 短连接 | 同步聊天，无 tool 执行 |

### ACP 协议帧

| Event | SSE 帧 |
|-------|--------|
| `connected` | `data: {"type":"connected","data":{"conn_id":"xxx"}}` |
| `assistant` | `data: {"type":"assistant","data":{"delta":"..."}}` |
| `tool_call` | `data: {"type":"tool_call","data":{"id":"x","name":"bash",...}}` |
| `tool_result` | `data: {"type":"tool_result","data":{"id":"x","success":true,...}}` |
| `error` | `data: {"type":"error","data":{"message":"..."}}` |
| `done` | `data: {"type":"done","data":{}}` |

`done` 帧标志一次 ACP 请求的 LLM 处理完全结束（含 tool 多轮对话）。

## SessionState / conn_id / SseFrameQueue 关系

### 数据结构

```cpp
unordered_map<string, SessionState> sessions_;  // session_id → SessionState

struct SessionState {
    unordered_map<string, shared_ptr<SseFrameQueue>> conns;  // conn_id → queue
    atomic<bool> processing;                                   // LLM 并发锁
};
```

- **session** ↔ **conn_id**：一对多（一个 session 多个 SSE 连接）
- **conn_id** ↔ **SseFrameQueue**：一对一

### 生命周期

```
1. SSE 连接建立
   handle_acp_stream("session_A"):
     → queue = make_shared<SseFrameQueue>()
     → conn_id = generate_conn_id()
     → sessions_["session_A"].conns[conn_id] = queue
     → queue->push(connected_frame)      // 告知客户端 conn_id
     → 进入 chunked provider 循环等待:
         while (true):
           auto frame = queue->pop()     // 阻塞等待
           sink.write(frame)             // 写到 TCP

2. ACP 请求处理
   handle_acp("session_A", conn_id):
     run_acp_loop_broadcast → broadcast(frame):
       sessions_["session_A"].conns[conn_id]->push(frame)
       → 唤醒 SSE 线程 pop() → sink.write() → 客户端

3. 切换 session（不断开 SSE）
   handle_acp_switch({conn_id, new_session}):
     → conn_id 从旧 session 移除
     → 插入 sessions_[new_session].conns[conn_id] = queue
     → queue->push(connected_frame)       // 确认切换完毕

4. 断开
   cleanup_connection(sid, conn_id):
     → sessions_[sid].conns.erase(conn_id)
     → 若 session 无其他连接, sessions_.erase(sid)
```

### 线程模型

```
  ACP 线程 (detached)                 SSE 线程 (httplib)
     │                                      │
     │ broadcast(frame)                     │
     │   ↓                                  │  queue->pop()
     │   sessions_[sid].conns[cid]->push()  │    → cv_.wait(lock)
     │   → lock → 入队 → notify_one() ──────►    → lock → 取出 → unlock
     │   → unlock                          │    → sink.write() → TCP
```

ACP 线程不直接写 TCP，只入队。mutex + condition_variable 同步。

## 技术选型

| 类别 | 库 | 版本 | 管理 |
|------|-----|------|------|
| HTTP | cpp-httplib | 0.47.0 [openssl] | vcpkg |
| JSON | nlohmann/json | 3.12.0 | vcpkg |
| CLI | CLI11 | 2.6.2 | vcpkg |
| 配置 | toml++ | 3.4.0 | vcpkg |
| SSL | OpenSSL | 3.6.3 | vcpkg |
| 异步 IO | standalone asio | 1.32.0 | vcpkg |
| 数据库 | SQLite3 | 3.45.1 | 系统自带 |
| C++ | C++20 | | |
| 构建 | CMake 3.20+ | | |
| 包管理 | vcpkg manifest (6 包) | | |

## 项目目录

```
opencode-cpp/
├── CMakeLists.txt / vcpkg.json
├── ARCHITECTURE.md / opencode-cpp-design.md / plan.md
│
├── packages/
│   ├── cli/src/main.cpp           # connect() + send_async() + send()
│   │       tui.h/cpp              # FTXUI TUI + session overlay
│   ├── server/src/
│   │   ├── server.h/cpp             # handle_acp_stream (conn_id), handle_acp_switch
│   │   └── main.cpp                 # handle_acp (conn_id routing)
│   ├── llm/src/
│   │   ├── types.h / acp.h          # ACP 协议帧定义 + connected 事件
│   │   ├── acp_client.h/cpp         # send() / connect() / send_async()
│   │   ├── session_store.h/cpp
│   │   ├── context_source.h/cpp
│   │   ├── tool.h / tool_registry.h / tools/
│   │   └── log.h
│   ├── plugin/
│   │   ├── include/
│   │   │   ├── plugin.h               # C ABI 接口
│   │   │   ├── plugin_loader.h/cpp     # dlopen 加载器
│   │   │   └── plugin_tool.h           # C → Tool 适配器
│   │   └── CMakeLists.txt
│   └── util/src/config.h/cpp
│
├── config/config.toml
└── bot/feishu_bot.py
```

## Client API

| 方法 | 行为 |
|------|------|
| `connect(sid, cbs)` | `GET /stream/{sid}?keepalive=1` → SSE → 后台线程接收推送 |
| `send_async(req)` | `POST /acp` (带 conn_id) → 202 → 无阻塞 |
| `send(req, cbs)` | `POST /acp` → 拿 session_id → `GET /stream/{sid}` → 阻塞读 SSE 直到 done → 返回 |

## LLM 并发控制

```cpp
struct SessionState {
    map<string, shared_ptr<SseFrameQueue>> conns;
    mutex mutex;
    atomic<bool> processing{false};  // 防止同一 session 并发 LLM 运行
};
```

- `handle_acp` 中检查 `processing.exchange(true)`
- 已有 LLM 在处理则跳过新的 ACP 请求
- `run_acp_loop_broadcast` 完成后 `processing = false`

## SessionStore (SQLite)

| 表 | 用途 |
|---|------|
| `sessions` | id, metadata, created_at, updated_at |
| `messages` | session_id, role, content, timestamp |
| `context_snapshots` | session_id, key, value, rendered |

## Plugin 系统 (v0.8.0)

### C ABI 接口

插件实现为动态库 `.so`，通过 C ABI 接口与宿主通信，`dlopen` 加载。

| 回调 | 说明 |
|------|------|
| `register_tool(name, desc, params, execute_fn, ctx)` | 注册自定义工具 |
| `log(level, msg)` | 插件日志 |

### 加载流程

```
Server 启动
  └─ PluginLoader::load_directory(CODIS_PLUGIN_DIR)
       ├─ 扫描 *.so → dlopen()
       ├─ dlsym("plugin_init")
       ├─ 注入 CodisAPI{register_tool, log}
       └─ plugin_init(&api, config_json)
            └─ api->register_tool("my_tool", ...)
                 └─ PluginTool 适配器 → tool_registry_.register_tool()
```

### 插件示例 (C)

```c
int plugin_init(const CodisAPI* api, const char* config_json) {
    api->register_tool("my_tool", "desc", param_json, execute_fn, NULL);
    return 0;
}
void plugin_shutdown(void) { }
```

### 文件

| 文件 | 说明 |
|------|------|
| `packages/llm/src/plugin.h` | C ABI 接口定义 |
| `packages/llm/src/plugin_loader.h/cpp` | dlopen 加载器 |
| `packages/llm/src/plugin_tool.h` | C 回调 → Tool 接口适配器 |
| `plugins/echo_plugin.c` | 示例插件 |

## Phase 演进

| Phase | 版本 | 交付 |
|-------|------|------|
| 1-7 | v0.1.0-v0.7.0 | 基础架构 |
| 8 | v0.8.0 | Plugin 系统 (C ABI) |
| 9 | v0.9.0 | EventBus: pub/sub 解耦 (已废弃) |
| 10 | v0.10.0 | 长 TCP: SSE stream + keepalive + conn_id |
