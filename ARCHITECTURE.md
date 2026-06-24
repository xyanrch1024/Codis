# Codis C++ C/S 架构设计

## 总体拓扑

```
┌──────────────┐   ACP + SSE (HTTP)   ┌─────────────────────────┐
│  codis        │ ◄─────────────────► │  codis-server             │
│  (CLI/TUI)   │   text/event-stream  │  (后台守护进程)            │
│              │                      │                          │
│  send()      │   POST /api/v1/acp   │  ├─ activeSessions       │
│  connect()   │   ─────────────────  │  │   session → {clients} │
│  后台SSE线程  │   ◄─ SSE broadcast   │  ├─ ProviderRegistry     │
│              │                      │  ├─ ToolRegistry (6)     │
│  交互命令:    │                      │  ├─ SystemContext (6)    │
│  /sessions   │                      │  ├─ SessionStore(SQLite) │
│  /session id │                      │  └─ Logger               │
│  /clear      │                      │                          │
└──────────────┘                      └─────────────────────────┘
```

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
| 通信协议 | **ACP** over HTTP SSE | | |

## 项目目录

```
opencode-cpp/
├── CMakeLists.txt / vcpkg.json
├── ARCHITECTURE.md / opencode-cpp-design.md / plan.md
│
├── packages/
│   ├── cli/src/main.cpp             # CLI + 后台 SSE + 历史同步
│   ├── server/src/                  # HTTP 守护进程
│   ├── llm/src/
│   │   ├── types.h                  # Message + ChatRequest(session_id)
│   │   ├── session_store.h/cpp      # SQLite CRUD
│   │   ├── context_source.h/cpp     # SystemContext + 6 sources
│   │   ├── tool.h / tool_registry.h / tools/  # 6 工具
│   │   ├── acp.h / acp_client.h/cpp # ACP 协议 + 客户端 (send/connect)
│   │   └── log.h
│   └── util/src/config.h/cpp        # ProviderConfig (api_key_env)
│
├── config/config.toml               # env vars only
└── bin/ / scripts/
```

## activeSessions — 多 Client 实时共享

```
struct ActiveSession {
    vector<weak_ptr<SseFrameQueue>> clients;  // 所有监听者
    atomic<bool> processing;
};

map<string, ActiveSession> active_sessions_;  // shared_mutex
```

### attach_to_session 三路分支

```
POST /api/v1/acp {session_id, messages}
  │
  ├─ processing=true → 加入监听 + 同步历史 (不触发 LLM)
  ├─ messages 无 user 内容 → 同步历史 + 加入监听 + 不发 done (view-only)
  └─ 有新 user 消息 → 同步历史 + 加入监听 + 启动 LLM + 广播
```

### Client 双模式

| 方法 | 行为 | 用途 |
|------|------|------|
| `send(req, cb)` | 阻塞 POST → 解析 SSE body → 返回 | 发送消息，等待回复 |
| `connect(sid, cb)` | 后台线程循环 POST + 解析 SSE | 实时接收其他 client 的广播 |

### 实时同步流程

```
Client A: send("hello") → LLM runs → broadcast
  ├─ queue_A → SSE → Client A 即时显示
  └─ queue_B → SSE → Client B 后台线程接收 → 实时渲染

Client B: connect(session_id)
  ├─ 同步历史消息
  ├─ 保持 SSE 连接
  └─ 实时接收 broadcast
```

## CLI 启动体验

```bash
./opencode -i                    # 自动恢复最后 session + 显示历史
./opencode -i -S <session_id>   # attach 到指定 session
```

```
╔══════════════════════════════════════════╗
║  Codis Client  v0.7.0                  ║
║  Server:   localhost:8711               ║
║  Session:  2dd48b8c...                  ║
╚══════════════════════════════════════════╝
── 3 messages loaded from session ──
You: hello
AI: Hello! How can I help?
──────────────────────────────────────
Commands: /exit /sessions /session <id> /clear

>
```

## ACP 协议

| Event | SSE 帧 |
|-------|--------|
| `assistant` | `data: {"type":"assistant","data":{"delta":"..."}}` |
| `tool_call` | `data: {"type":"tool_call","data":{"id":"x","name":"bash",...}}` |
| `tool_result` | `data: {"type":"tool_result","data":{"id":"x","success":true,...}}` |
| `error` | `data: {"type":"error","data":{"message":"..."}}` |
| `done` | `data: {"type":"done","data":{}}` |

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | 健康 + tools + providers |
| `POST` | `/api/v1/acp` | ACP SSE + session_id + 多 client |
| `GET` | `/api/v1/sessions` | 列出所有 |
| `GET` | `/api/v1/sessions/:id` | 获取 + 消息历史 |
| `DELETE` | `/api/v1/sessions/:id` | 删除 |

## 配置（仅环境变量）

```toml
[[providers]]
name = "glm"
api_key_env = "GLM_API_KEY"
model = "glm-4.5-flash"
base_url = "https://open.bigmodel.cn/api/paas/v4"
```

```bash
export GLM_API_KEY="..."
./opencode-server -p 8711 -c config/config.toml
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
| 7 | v0.7.0 | **activeSessions 多 client 实时同步 + 后台 SSE** |
| 8 | v0.8.0 | ReAct + RAG |
