# Codis C++ C/S 架构设计

## 总体拓扑

```
┌──────────────┐  fire-and-forget (REST)  ┌─────────────────────────┐
│  codis        │ ◄─────────────────────► │  codis-server             │
│  (CLI/TUI)   │  SSE long-lived stream   │  (后台守护进程)            │
│              │                          │                          │
│  send_async()│  POST /api/v1/acp        │  ├─ activeSessions       │
│  connect()   │  ──────────────────────► │  │   session → {clients} │
│  后台SSE线程  │  GET /api/v1/acp/stream   │  ├─ ProviderRegistry     │
│              │ ◄════ SSE keep-alive ═══ │  ├─ ToolRegistry (6)     │
│              │                          │  ├─ SystemContext (6)    │
│  交互命令:    │                          │  ├─ SessionStore(SQLite) │
│  /sessions   │                          │  └─ Logger               │
│  /session id │                          │                          │
│  /clear      │                          │                          │
└──────────────┘                          └─────────────────────────┘
```

## 通信架构

### 长 TCP 连接

```
1 个 SSE 长连接 (贯穿整个 session 生命周期)

Client                            Server
  │                                │
  │  GET /api/v1/acp/stream/{id}   │  建立 SSE 长连接
  │ ═══════ keep-alive ══════════ │
  │  ←── history frames ──────── │  历史消息
  │  ←── broadcast tokens ────── │  其他 client 的消息
  │                                │
  │  POST /api/v1/acp              │  fire-and-forget 发送消息
  │  {session_id, messages}        │  → 202 Accepted
  │                                │  → LLM → broadcast → SSE stream
  │  ←── assistant frames ────── │  实时 token
  │  ←── done ─────────────────── │
  │                                │
  │  POST /api/v1/acp              │  下一条消息...
  │  ←── assistant frames ────── │
```

### 端点对比

| 端点 | 方法 | 连接 | 说明 |
|------|------|------|------|
| `/api/v1/acp/stream/{id}` | GET | **长连接** | SSE 流，持续推送，客户端用 `connect()` |
| `/api/v1/acp` | POST | 短连接 | fire-and-forget，触发 LLM，返回 202 |

## 技术选型

| 类别 | 库 | 版本 | 管理 |
|------|-----|------|------|
| HTTP 客户端/服务端 | **cpp-httplib** | 0.47.0 [openssl] | vcpkg |
| JSON | **nlohmann/json** | 3.12.0 | vcpkg |
| CLI 参数 | **CLI11** | 2.6.2 | vcpkg |
| 配置 (TOML) | **toml++** | 3.4.0 | vcpkg |
| SSL/TLS | **OpenSSL** | 3.6.3 | vcpkg |
| 异步 IO + 信号 | **standalone asio** | 1.32.0 | vcpkg |
| 数据库 | **SQLite3** | 3.45.1 | 系统自带 |
| C++ 标准 | **C++20** | | |
| 构建 | **CMake** 3.20+ | | |
| 包管理 | **vcpkg** manifest (6 包) | | |

## 项目目录

```
opencode-cpp/
├── CMakeLists.txt / vcpkg.json
├── ARCHITECTURE.md / opencode-cpp-design.md / plan.md
│
├── packages/
│   ├── cli/src/main.cpp             # CLI: connect() + send_async()
│   ├── server/src/                  # HTTP 守护进程
│   │   ├── main.cpp                 # -p port -c config
│   │   ├── server.h                 # activeSessions + 所有子系统
│   │   └── server.cpp               # handle_acp(fire-and-forget) + handle_acp_stream(SSE)
│   ├── llm/src/
│   │   ├── types.h                  # Message + ChatRequest(session_id)
│   │   ├── session_store.h/cpp      # SQLite CRUD + create_session_with_id
│   │   ├── context_source.h/cpp     # SystemContext + 6 sources
│   │   ├── tool.h / tool_registry.h / tools/  # 6 工具
│   │   ├── acp.h / acp_client.h/cpp # ACP 协议 + 客户端 (send_async/connect)
│   │   └── log.h
│   └── util/src/config.h/cpp        # ProviderConfig (api_key_env)
│
├── config/config.toml               # env vars only
└── bin/ / scripts/
```

## Client API

| 方法 | 行为 | 用途 |
|------|------|------|
| `connect(sid, cbs)` | `GET /stream/{sid}` → SSE 长连接 → 后台线程接收 | 建立长连接，接收所有 LLM 响应和广播 |
| `send_async(req)` | `POST /acp` → 202 → 立即返回 | fire-and-forget 发送消息 |
| `send(req, cbs)` | `POST /acp` → 阻塞读取 SSE body → 返回 | 同步发送消息（旧） |

## activeSessions — 多 Client 共享

```
struct ActiveSession {
    vector<weak_ptr<SseFrameQueue>> clients;  // SSE stream 监听者
    atomic<bool> processing;
};

map<string, ActiveSession> active_sessions_;  // unique_lock 保护
```

### 广播流程

```
handle_acp (fire-and-forget):
  ├─ 保存 user 消息到 SessionStore
  └─ detach LLM thread → run_acp_loop_broadcast()
       │
       ├─ LLM stream → broadcast(assistant_frame)
       ├─ tool.execute → broadcast(tool_result_frame)
       └─ done → broadcast(done_frame) + cleanup
```

## ACP 协议

| Event | SSE 帧 |
|-------|--------|
| `assistant` | `data: {"type":"assistant","data":{"delta":"..."}}` |
| `tool_call` | `data: {"type":"tool_call","data":{"id":"x","name":"bash",...}}` |
| `tool_result` | `data: {"type":"tool_result","data":{"id":"x","success":true,...}}` |
| `error` | `data: {"type":"error","data":{"message":"..."}}` |
| `done` | `data: {"type":"done","data":{}}` |

## CLI 启动体验

```bash
./opencode -i                    # 恢复最后 session + 显示历史
./opencode -i -S <session_id>   # attach 到指定 session
./opencode -i -w                 # watch mode: 只接收广播，不输入
```

```
╔══════════════════════════════════════════╗
║  Codis Client  v0.8.0                  ║
║  Server:   localhost:8711               ║
║  Session:  00001111...                  ║
╚══════════════════════════════════════════╝
── 3 messages loaded from session ──
You: hello
AI: Hi! How can I help?
──────────────────────────────────────
Commands: /exit /sessions /session <id> /clear

> 用户输入 → send_async → SSE stream 实时返回
```

## Phase 演进

| Phase | 版本 | 交付 |
|-------|------|------|
| 1 | v0.1.0 | CLI + 非流式 LLM |
| 2 | v0.2.0 | C/S REST |
| 3 | v0.3.0 | ACP + SSE |
| 3.1 | v0.3.1 | 多 Provider + 日志 |
| 4 | v0.4.0 | Tool Registry |
| 5 | v0.5.0 | SQLite + SystemContext |
| 6 | v0.6.0 | Session CLI |
| 7 | v0.7.0 | Multi-client 共享 |
| 8 | v0.8.0 | **长 TCP 连接: SSE stream + fire-and-forget** |
| 9 | v0.9.0 | ReAct + RAG |
