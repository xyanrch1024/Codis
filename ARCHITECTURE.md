# Codis C++ C/S 架构设计

## 总体拓扑

```
┌──────────────┐  fire-and-forget (REST)  ┌─────────────────────────────────┐
│  codis        │ ◄─────────────────────► │  codis-server                   │
│  (CLI/TUI)   │  WebSocket 推送          │  (后台守护进程)                  │
│              │                          │                                 │
│  send_async()│  POST /api/v1/acp        │  ├─ SessionState (per session)  │
│  connect()   │  ──────────────────────► │  │   conns: map<conn_id, queue> │
│  后台WS线程   │  WS /api/v1/acp/ws/{id}  │  │   processing: atomic<bool>   │
│              │ ◄══ keep-alive WS ═════  │  │   pending: deque<ChatRequest>│
│              │                          │  ├─ ProviderRegistry           │
│  交互命令:    │                          │  ├─ ToolRegistry (6)           │
│  /sessions   │                          │  ├─ SystemContext (6)          │
│  /session id │                          │  ├─ SessionStore (SQLite)      │
│  /clear      │                          │  └─ Logger                     │
│  /clearsessions                         │                                 │
└──────────────┘                          └─────────────────────────────────┘
```

## 通信架构 (v0.10.0+)

### SessionState — 每 session 的直接队列广播

EventBus 已从推送路径移除，改为 SessionState 管理每个 session 下的所有连接队列：

```
                     SessionState (per session_id)
                 ┌──────────────────────────────────────┐
                 │  conns: map<conn_id, FrameQueue>     │
                 │  processing: atomic<bool>            │
                 │  pending: deque<ChatRequest>         │
                 │  mutex                               │
                 │                                      │
LLM Worker ─────┤  broadcast(frame, conn_id)            │
  (run_acp_    │                                      │
   loop_       │  conn_id 非空 → push target queue     │──→ WS A
   broadcast)  │  conn_id 为空 → push all queues       │──→ WS B
                 │                                      │
                 └──────────────────────────────────────┘
```

- 每个 WS 连接分配唯一 `conn_id`
- ACP 请求可携带 `conn_id` 精确路由 LLM 输出
- `conn_id` 为空时广播到该 session 的所有连接（向后兼容）
- 断开时自动清理：`ws.read()` 返回 Fail 触发 `cleanup_connection()`
- **in-flight 排队**：LLM 处理期间新消息进入 `pending`，当前轮 `done` 后按序自动补跑，避免静默丢弃

### WebSocket 长连接 (keepalive)

```
Client                                    Server
  │                                         │
  │  WS /api/v1/acp/ws/{id}               │  allocate conn_id
  │                                        │  push connected frame
  │  ←── {"type":"connected","conn_id":"x"}  │
  │                                        │
  │  POST /api/v1/acp                      │  fire-and-forget → LLM
  │  {"session_id","messages","conn_id"}   │  → 202 Accepted
  │                                        │  → run_acp_loop_broadcast()
  │  ←── assistant frames ──────────────  │  → broadcast to conns[conn_id]
  │  ←── reasoning frames ──────────────  │
  │  ←── tool_call ──────────────────────  │
  │  ←── tool_result ────────────────────  │
  │  ←── {"type":"done"} ────────────────  │
  │                                        │  keepalive: 不关闭连接
  │  POST /api/v1/acp                      │  下一轮 ACP
  │  ←── assistant frames ──────────────  │
  │  ←── done ───────────────────────────  │
  │                                        │
  │  disconnect                             │  ws.read() fail
  │                                        │  → cleanup_connection()
```

WS 连接不会因空闲断开（`read_timeout` 不设，心跳由 httplib 处理）。一个连接持续复用，逐轮接收 ACP 推送。

### 端点

| 端点 | 方法 | 连接 | 说明 |
|------|------|------|------|
| `/api/v1/acp/ws/{id}` | WS | 长连接 | WebSocket 推送，分配 conn_id |
| `/api/v1/acp` | POST | 短连接 | fire-and-forget，传 conn_id 精准路由 |
| `/api/v1/acp/switch` | POST | 短连接 | conn_id 切换到其它 session |
| `/api/v1/chat` | POST | 短连接 | 同步聊天，无 tool 执行 |

### ACP 协议帧 (WebSocket，裸 JSON，无 SSE 信封)

| Event | WS 帧 |
|-------|--------|
| `connected` | `{"type":"connected","data":{"conn_id":"xxx"}}` |
| `assistant` | `{"type":"assistant","data":{"delta":"..."}}` |
| `reasoning` | `{"type":"reasoning","data":{"delta":"..."}}` |
| `tool_call` | `{"type":"tool_call","data":{"id":"x","name":"bash",...}}` |
| `tool_result` | `{"type":"tool_result","data":{"id":"x","success":true,...}}` |
| `error` | `{"type":"error","data":{"message":"..."}}` |
| `done` | `{"type":"done","data":{}}` |

`done` 帧标志一次 ACP 请求的 LLM 处理完全结束（含 tool 多轮对话）。

## SessionState / conn_id / FrameQueue 关系

### 数据结构

```cpp
unordered_map<string, SessionState> sessions_;  // session_id → SessionState

struct SessionState {
    unordered_map<string, shared_ptr<FrameQueue>> conns;  // conn_id → queue
    atomic<bool> processing{false};                       // LLM 并发锁
    deque<ChatRequest> pending;                           // 处理期间到达的请求
};
```

- **session** ↔ **conn_id**：一对多（一个 session 多个 WS 连接）
- **conn_id** ↔ **FrameQueue**：一对一

### 生命周期

```
1. WS 连接建立
   handle_acp_ws("session_A"):
     → queue = make_shared<FrameQueue>()
     → conn_id = generate_conn_id()
     → sessions_["session_A"].conns[conn_id] = queue
     → queue->push(connected_frame)      // 告知客户端 conn_id
     → 启动 sender 线程:
         while (true):
           auto frame = queue->pop()     // 阻塞等待
           ws.send(frame)                // 写到 TCP
     → 进入 ws.read() 循环等待断开

2. ACP 请求处理
   handle_acp("session_A", conn_id):
     → processing.exchange(true) 为 false 则启动 run_acp_loop_broadcast
     → 若为 true，请求入 pending 队列，当前轮结束后补跑
   run_acp_loop_broadcast → broadcast(frame):
     sessions_["session_A"].conns[conn_id]->push(frame)
     → 唤醒 sender 线程 pop() → ws.send() → 客户端

3. 切换 session（不断开 WS）
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
  ACP 线程 (detached)                 WS sender 线程
     │                                      │
     │ broadcast(frame)                     │
     │   ↓                                  │  queue->pop()
     │   sessions_[sid].conns[cid]->push()  │    → cv_.wait(lock)
     │   → lock → 入队 → notify_one() ──────►    → lock → 取出 → unlock
     │   → unlock                          │    → ws.send() → TCP
```

ACP 线程不直接写 TCP，只入队。mutex + condition_variable 同步。

## 端口冲突保护

httplib 默认 socket 选项使用 `SO_REUSEPORT`，会导致两个 server 进程同时绑定同一端口，
内核把新连接分发给不同进程 —— WS 长连接和 HTTP POST 落在不同进程时消息会静默丢失。

`OpenCodeServer` 构造函数中覆盖 socket 选项为仅 `SO_REUSEADDR`：

```cpp
server_->set_socket_options([](int sock) {
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
});
```

第二个实例 bind 直接失败并 `_Exit(1)`（日志打印明确错误），不再静默共享端口。

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
│   ├── cli/src/main.cpp           # connect() + send_async()
│   │       tui.h/cpp              # FTXUI TUI + session overlay
│   ├── server/src/
│   │   ├── server.h/cpp             # handle_acp_ws (conn_id), handle_acp_switch
│   │   └── main.cpp                 # handle_acp (conn_id routing)
│   ├── llm/src/
│   │   ├── types.h / acp.h          # ACP 协议帧定义 + connected 事件
│   │   ├── acp_client.h/cpp         # connect() / send_async()
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
| `connect(sid, cbs)` | `WS /api/v1/acp/ws/{sid}` → 后台线程接收推送，断线自动重连（指数退避，最多 10 次） |
| `send_async(req)` | `POST /acp` (带 conn_id) → 202 → 无阻塞，回复通过 WS 到达 |

## LLM 并发控制

```cpp
struct SessionState {
    map<string, shared_ptr<FrameQueue>> conns;
    deque<ChatRequest> pending;        // 处理期间到达的请求
    mutex mutex;
    atomic<bool> processing{false};    // 防止同一 session 并发 LLM 运行
};
```

- `handle_acp` 中检查 `processing.exchange(true)`
- 已有 LLM 在处理则将新请求入 `pending` 队列（不再跳过）
- `run_acp_loop_broadcast` 完成后，若 `pending` 非空则保持 `processing` 并用新请求立即补跑下一轮；否则置 `processing = false`
- `broadcast` 中目标 conn_id 在 session 中不存在时打 WARN，便于诊断"服务端有消息但客户端收不到"

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
