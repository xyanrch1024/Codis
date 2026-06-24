# Codis C++ C/S 架构设计

## 总体拓扑

```
┌──────────────┐  fire-and-forget (REST)  ┌─────────────────────────┐
│  codis        │ ◄─────────────────────► │  codis-server             │
│  (CLI/TUI)   │  SSE long-lived stream   │  (后台守护进程)            │
│              │                          │                          │
│  send_async()│  POST /api/v1/acp        │  ├─ EventBus (pub/sub)   │
│  connect()   │  ──────────────────────► │  │   topic: session:*:   │
│  后台SSE线程  │  GET /api/v1/acp/stream   │  │     event             │
│              │ ◄════ SSE keep-alive ═══ │  ├─ ProviderRegistry     │
│              │                          │  ├─ ToolRegistry (6)     │
│  交互命令:    │                          │  ├─ SystemContext (6)    │
│  /sessions   │                          │  ├─ SessionStore(SQLite) │
│  /session id │                          │  └─ Logger               │
│  /clear      │                          │                          │
└──────────────┘                          └─────────────────────────┘
```

## 通信架构 (v0.9.0)

### EventBus — 解耦的发布/订阅

```
                         EventBus (全局单例)
                    ┌─────────────────────────────┐
                    │  topic → [handler_id, handler] │
                    │                             │
  LLM Worker ──────►│  publish("s:123:event", f)  │
                    │       │                     │
                    │       ├─► handler_A(f)       │──► queue_A → SSE A
                    │       ├─► handler_B(f)       │──► queue_B → SSE B
                    │       └─► handler_log(f)     │──► 审计日志 (未来)
                    │                             │
  Tool.execute ────►│  publish("s:123:event", f)  │
                    │                             │
                    └─────────────────────────────┘
```

- `subscribe(topic, handler)`: SSE stream 注册回调
- `publish(topic, frame)`: LLM Worker 发送帧
- `unsubscribe(topic, id)`: SSE stream 断开时取消
- `shared_mutex`: 读多写少，performance near lock-free

### 长 TCP 连接

```
1 个 SSE 长连接 (贯穿整个 session 生命周期)

Client                            Server
  │                                │
  │  GET /api/v1/acp/stream/{id}   │  SSE stream: subscribe EventBus
  │ ═══════ keep-alive ══════════ │
  │  ←── history frames ──────── │  历史消息
  │                                │
  │  POST /api/v1/acp              │  fire-and-forget → LLM
  │  {session_id, messages}        │  → 202 Accepted
  │                                │  → LLM → publish EventBus
  │  ←── assistant frames ────── │  实时 token
  │  ←── done ─────────────────── │
```

### 端点

| 端点 | 方法 | 连接 | 说明 |
|------|------|------|------|
| `/api/v1/acp/stream/{id}` | GET | 长连接 | SSE stream, subscribe EventBus |
| `/api/v1/acp` | POST | 短连接 | fire-and-forget, 202 |

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
| EventBus | **std::shared_mutex** | C++17 | 标准库 |
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
│   ├── server/src/                # 2 ACP 端点 + EventBus publish
│   ├── llm/src/
│   │   ├── event_bus.h            # 发布/订阅 (shared_mutex)
│   │   ├── types.h / acp.h / acp_client.h/cpp
│   │   ├── session_store.h/cpp
│   │   ├── context_source.h/cpp
│   │   ├── tool.h / tool_registry.h / tools/
│   │   └── log.h
│   └── util/src/config.h/cpp
│
├── config/config.toml
└── bin/ / scripts/
```

## EventBus API

```cpp
class EventBus {
    using Handler = function<void(const string& frame)>;

    uint64_t subscribe(topic, Handler);           // 订阅
    void unsubscribe(topic, subscription_id);      // 取消
    void publish(topic, frame);                    // 发布 (shared_lock)
    void clear(topic);                             // 清理
    size_t subscriber_count(topic);               // 订阅数
};
```

## Client API

| 方法 | 行为 |
|------|------|
| `connect(sid, cbs)` | `GET /stream/{sid}` → SSE → 后台线程接收 EventBus 推送 |
| `send_async(req)` | `POST /acp` → 202 → 无阻塞 |

## activeSessions — 处理中的 LLM 跟踪

```
ActiveSession { atomic<bool> processing }
  → 防止同一 session 并发 LLM 运行
```

## ACP 协议

| Event | SSE 帧 |
|-------|--------|
| `assistant` | `data: {"type":"assistant","data":{"delta":"..."}}` |
| `tool_call` | `data: {"type":"tool_call","data":{"id":"x","name":"bash",...}}` |
| `tool_result` | `data: {"type":"tool_result","data":{"id":"x","success":true,...}}` |
| `error` | `data: {"type":"error","data":{"message":"..."}}` |
| `done` | `data: {"type":"done","data":{}}` |

## Phase 演进

| Phase | 版本 | 交付 |
|-------|------|------|
| 1-7 | v0.1.0-v0.7.0 | 基础架构完成 |
| 8 | v0.8.0 | 长 TCP: SSE stream + fire-and-forget |
| 9 | v0.9.0 | **EventBus: pub/sub 解耦 LLM 与传输层** |
| 10 | v0.10.0 | ReAct + RAG |
