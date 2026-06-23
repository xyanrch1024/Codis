# OpenCode C++ C/S 架构设计

## 总体拓扑

```
┌──────────────┐   ACP + SSE (HTTP)   ┌─────────────────────────┐
│  codis        │ ◄─────────────────► │  codis-server             │
│  (CLI/TUI)   │   text/event-stream  │  (后台守护进程)            │
│              │                      │                          │
│  AcpClient   │   POST /api/v1/acp   │  ├─ ProviderRegistry     │
│  +Session Cmd│   ─────────────────  │  │   ├─ OpenAI            │
│              │   ◄─ SSE frames ──── │  │   ├─ DeepSeek          │
│  交互命令:    │                      │  │   └─ GLM/...          │
│  /sessions   │                      │  ├─ ToolRegistry (6)     │
│  /session    │                      │  ├─ SystemContext (6)    │
│  /clear      │                      │  ├─ SessionStore (SQLite)│
└──────────────┘                      │  ├─ ACP Loop             │
                                      │  ├─ SseFrameQueue        │
                                      │  └─ Logger               │
                                      └─────────────────────────┘
```

---

## 技术选型

| 类别 | 库 | 版本 | 包管理 |
|------|-----|------|--------|
| HTTP 客户端/服务端 | **cpp-httplib** | 0.47.0 [openssl] | vcpkg |
| JSON | **nlohmann/json** | 3.12.0 | vcpkg |
| CLI 参数 | **CLI11** | 2.6.2 | vcpkg |
| 配置 (TOML) | **toml++** | 3.4.0 | vcpkg |
| SSL/TLS | **OpenSSL** | 3.6.3 | vcpkg |
| 异步 IO + 信号 | **standalone asio** | 1.32.0 | vcpkg |
| 数据库 | **SQLite3** | 3.45.1 | 系统自带 |
| 日志 | **std::format** + std::mutex | C++20 | 标准库 |
| 子进程 | **fork/exec** + pipe | POSIX | 系统调用 |
| 文件/正则 | **std::filesystem / std::regex** | C++17 | 标准库 |
| C++ 标准 | **C++20** | | |
| 构建 | **CMake** 3.20+ | | |
| 包管理 | **vcpkg** manifest (6 包) | | |
| 通信协议 | **ACP** over HTTP SSE | | |

---

## 项目目录结构

```
opencode-cpp/
├── CMakeLists.txt
├── vcpkg.json
├── ARCHITECTURE.md / opencode-cpp-design.md / plan.md
│
├── packages/
│   ├── cli/src/main.cpp             # CLI + 会话管理命令
│   ├── server/src/                  # HTTP 守护进程
│   ├── llm/src/
│   │   ├── types.h                  # Message (tool_call 扩展)
│   │   ├── provider.h               # LLMProvider
│   │   ├── openai_compatible_provider.h/cpp
│   │   ├── provider_registry.h
│   │   ├── tool.h / tool_registry.h / tools/  # 6 工具
│   │   ├── session_store.h/cpp      # SQLite CRUD + list_info / set_title / search
│   │   ├── context_source.h/cpp     # SystemContext + 6 sources
│   │   ├── client.h/cpp             # HTTPS + SSE
│   │   ├── acp.h / acp_client.h/cpp # ACP 协议 + 客户端
│   │   └── log.h
│   └── util/src/config.h/cpp
│
├── config/config.toml
├── bin/ / scripts/
└── plan.md
```

---

## 会话管理

### 数据模型

```sql
sessions(id, created_at, updated_at, metadata)
  └─ metadata: JSON {"title": "..."}

messages(id, session_id FK, role, content, tool_call_id, tool_name, created_at)
```

### SessionStore API

```cpp
class SessionStore {
    // CRUD
    std::string create_session(metadata);
    std::optional<SessionData> load_session(id);
    void delete_session(id);

    // 增强查询
    std::vector<SessionInfo> list_sessions_info();
    void set_title(id, title);
    std::string get_last_session();
    int message_count(id);
    std::vector<std::string> search_sessions(query, limit);

    // 消息
    void append_message(session_id, Message);
    std::vector<Message> load_messages(session_id);
};
```

### SessionInfo

```json
{
  "id": "7b4ae684-092d-4fd9-a5cc-3ac2c285d862",
  "title": "重构登录模块",
  "message_count": 12,
  "created_at": 1782213328,
  "updated_at": 1782213328
}
```

### CLI 命令

| 命令 | 功能 |
|------|------|
| `/sessions` | 表格列出所有 session (ID/Msgs/Title) |
| `/session <id>` | 查看 session 详情 |
| `/session <id> use` | 切换到此 session，恢复历史消息 |
| `/session <id> del` | 删除 session |
| `/clear` | 清空当前对话上下文 |

### 自动恢复

启动时自动加载最后活跃的 session，未完成对话无缝接续。

---

## ACP Loop (含 System Context)

```
while turn < 10:
    turn == 1 → build_baseline() → system message
    turn >  1 → reconcile() → system message (if changed)

    1. LLM stream (with tools) → assistant frames
    2. extract_tool_calls(content)
       ├─ empty → break
       └─ has calls → execute → tool_result frames
    3. add results to message history
    4. continue
```

---

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | 健康 + tools + providers |
| `GET` | `/api/v1/info` | 服务信息 |
| `POST` | `/api/v1/chat` | 非流式聊天 |
| `POST` | `/api/v1/acp` | ACP SSE 流式 + 多轮 tool |
| `POST` | `/api/v1/sessions` | 创建会话 |
| `GET` | `/api/v1/sessions` | **列出所有会话 (含 title/msg_count)** |
| `GET` | `/api/v1/sessions/:id` | 获取会话 + 历史消息 |
| `DELETE` | `/api/v1/sessions/:id` | **删除会话** |
| `POST` | `/api/v1/sessions/:id/messages` | 添加消息 |

---

## Phase 演进

| Phase | 版本 | 关键交付 |
|-------|------|----------|
| 1 | v0.1.0 | 单体 CLI + 非流式 LLM |
| 2 | v0.2.0 | C/S REST + Session |
| 3 | v0.3.0 | ACP + SSE 实时流式 |
| 3.1 | v0.3.1 | 多 Provider + 日志 + SSL |
| 4 | v0.4.0 | Tool Registry + 6 工具 |
| 5 | v0.5.0 | SQLite 持久化 + System Context |
| 6 | v0.6.0 | **Session 管理 (list/restore/delete) + CLI 命令** |
| 7 | v0.7.0 | ReAct + RAG (见 plan.md) |
| 8 | v0.8.0 | Plugin 系统 (C ABI) |
