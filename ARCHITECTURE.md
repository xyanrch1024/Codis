# Codis C++ C/S 架构设计

## 总体拓扑

```
┌──────────────┐   ACP + SSE (HTTP)   ┌─────────────────────────┐
│  codis        │ ◄─────────────────► │  codis-server             │
│  (CLI/TUI)   │   text/event-stream  │  (后台守护进程)            │
│              │                      │                          │
│  命令:        │   POST /api/v1/acp   │  ├─ activeSessions (共享) │
│  /sessions   │   ─────────────────  │  │   session: {clients[]} │
│  /session id │   ◄─ SSE broadcast   │  ├─ ProviderRegistry     │
│  /clear      │                      │  ├─ ToolRegistry (6)     │
│              │                      │  ├─ SystemContext (6)    │
│  启动时:      │                      │  ├─ SessionStore(SQLite) │
│  同步历史     │                      │  └─ Logger               │
│  显示聊天     │                      │                          │
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
│   ├── cli/src/main.cpp             # CLI + 会话命令 + 启动历史同步
│   ├── server/src/                  # HTTP 守护进程
│   │   ├── main.cpp                 # -p port -c config
│   │   ├── server.h                 # activeSessions + 所有子系统
│   │   └── server.cpp               # 8 端点 + attach_to_session + broadcast
│   ├── llm/src/
│   │   ├── types.h                  # Message + ChatRequest (含 session_id)
│   │   ├── session_store.h/cpp      # SQLite CRUD + list_info / search
│   │   ├── context_source.h/cpp     # SystemContext + 6 sources
│   │   ├── tool.h / tool_registry.h / tools/  # 6 工具
│   │   ├── provider_registry.h      # 多 provider
│   │   ├── openai_compatible_provider.h/cpp
│   │   ├── client.h/cpp             # HTTPS + SSE
│   │   ├── acp.h / acp_client.h/cpp # ACP 协议 + 客户端
│   │   └── log.h
│   └── util/src/config.h/cpp        # ProviderConfig (api_key_env)
│
├── config/config.toml               # env vars only, no plaintext keys
├── bin/ / scripts/
└── plan.md
```

## activeSessions（多 Client 共享）

```
struct ActiveSession {
    vector<weak_ptr<SseFrameQueue>> clients;  // 所有监听者
    atomic<bool> processing;
};

map<string, ActiveSession> active_sessions_;  // shared_mutex 保护
```

### 三路 attach 逻辑

```
Client POST /api/v1/acp {session_id, messages}
  │
  ├─ session 正在 processing → 加入监听 + 同步历史 (不触发 LLM)
  ├─ session 空闲 + messages 无 user 内容 → 同步历史 + done (不触发 LLM)
  └─ session 空闲 + 有 user 消息 → 同步历史 + 启动 LLM + 广播
```

### LLM 广播

```
run_acp_loop_broadcast():
  ├─ LLM stream → token
  │   └─ broadcast(assistant_frame(delta))
  │       └─ 遍历 active.clients → queue->push(frame)
  ├─ tool_call → tool.execute() → broadcast(tool_result_frame)
  └─ done → broadcast(done_frame) → erase from active_sessions
```

## 会话管理

### CLI 命令

| 命令 | 功能 |
|------|------|
| `/sessions` | 表格列出所有 session |
| `/session <id>` | 切换 session |
| `/session <id> use` | 恢复历史到当前 |
| `/session <id> del` | 删除 |
| `/clear` | 清空当前 |

### CLI 启动

```bash
# 自动恢复最后 session + 显示历史聊天
./opencode -i

# attach 到指定 session
./opencode -i -S "2dd48b8c-dd8d-4930-83fd-af23d44fa56b"
```

启动时自动 `GET /api/v1/sessions/:id` 拉取历史并渲染聊天格式。

### 配置（仅环境变量）

```toml
[[providers]]
name = "deepseek"
api_key_env = "DEEPSEEK_API_KEY"   # 不在文件里写明文 key
model = "deepseek-chat"
```

```bash
export DEEPSEEK_API_KEY="sk-..."
export GLM_API_KEY="..."
```

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | 健康 + tools + providers |
| `POST` | `/api/v1/acp` | ACP SSE + 多轮 tool + multi-client |
| `GET` | `/api/v1/sessions` | 列出所有 |
| `GET` | `/api/v1/sessions/:id` | 获取 + 消息历史 |
| `DELETE` | `/api/v1/sessions/:id` | 删除 |
| `POST` | `/api/v1/sessions/:id/messages` | 添加消息 |

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
| 1 | v0.1.0 | CLI + 非流式 LLM |
| 2 | v0.2.0 | C/S REST + Session |
| 3 | v0.3.0 | ACP + SSE |
| 3.1 | v0.3.1 | 多 Provider + 日志 + SSL |
| 4 | v0.4.0 | Tool Registry + 6 工具 |
| 5 | v0.5.0 | SQLite + System Context |
| 6 | v0.6.0 | Session 管理 + CLI 命令 |
| 7 | v0.7.0 | **activeSessions 多 client 共享 + 历史同步 + 广播** |
| 8 | v0.8.0 | ReAct + RAG (见 plan.md) |
