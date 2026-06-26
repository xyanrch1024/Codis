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
  │                                        │  push history (if no skip_history)
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
| `?skip_history=1` | 不推送历史消息（`send()` 使用） |
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
│   ├── server/src/
│   │   ├── server.h/cpp             # handle_acp_stream (conn_id)
│   │   └── main.cpp                 # handle_acp (conn_id routing)
│   ├── llm/src/
│   │   ├── types.h / acp.h          # ACP 协议帧定义 + connected 事件
│   │   ├── acp_client.h/cpp         # send() / connect() / send_async()
│   │   ├── session_store.h/cpp
│   │   ├── context_source.h/cpp
│   │   ├── tool.h / tool_registry.h / tools/
│   │   └── log.h
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
| `send(req, cbs)` | `POST /acp` → 拿 session_id → `GET /stream/{sid}?skip_history=1` → 阻塞读 SSE 直到 done → 返回 |

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

## Phase 演进

| Phase | 版本 | 交付 |
|-------|------|------|
| 1-7 | v0.1.0-v0.7.0 | 基础架构 |
| 8 | v0.8.0 | 长 TCP: SSE stream + fire-and-forget |
| 9 | v0.9.0 | EventBus: pub/sub 解耦 (已废弃) |
| 10 | v0.10.0 | **SessionState 直接队列广播 + conn_id 精准路由 + keepalive** |
