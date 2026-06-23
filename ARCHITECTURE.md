# OpenCode C++ C/S 架构设计

## 总体拓扑

```
┌──────────────┐   ACP + SSE (HTTP)   ┌─────────────────────────┐
│  opencode     │ ◄─────────────────► │  opencode-server          │
│  (CLI/TUI)   │   text/event-stream  │  (后台守护进程)            │
│              │                      │                          │
│  AcpClient   │   POST /api/v1/acp   │  ├─ ProviderRegistry     │
│  事件回调驱动  │   ─────────────────  │  │   ├─ OpenAI            │
│              │   ◄─ SSE frames ──── │  │   ├─ DeepSeek          │
│              │                      │  │   └─ Groq/...          │
└──────────────┘                      │  ├─ ToolRegistry         │
                                      │  │   bash read write       │
                                      │  │   edit glob grep         │
                                      │  ├─ SystemContext          │
                                      │  │   date platform git     │
                                      │  │   working_dir tools     │
                                      │  │   project_instructions  │
                                      │  ├─ SessionStore (SQLite)  │
                                      │  ├─ ACP Loop (多轮工具)     │
                                      │  ├─ SseFrameQueue          │
                                      │  └─ Logger (log.h)         │
                                      └─────────────────────────┘
                                               │
                                        ┌──────┴──────────────┐
                                        │  LLM APIs            │
                                        │  OpenAI / DeepSeek   │
                                        └─────────────────────┘
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
| 构建 | **CMake** 3.20+ | | |
| 包管理 | **vcpkg** manifest (6 包) | | |
| C++ 标准 | **C++20** | | |
| 通信协议 | **ACP** over HTTP SSE | | |

---

## 项目目录结构

```
opencode-cpp/
├── CMakeLists.txt
├── vcpkg.json                     # asio, httplib[openssl], nlohmann-json, CLI11, toml++, openssl
├── ARCHITECTURE.md                # 本文档
│
├── packages/
│   ├── cli/src/main.cpp           # CLI → AcpClient → 事件回调
│   │
│   ├── server/src/                # HTTP 守护进程
│   │   ├── main.cpp               # -p port -c config + 信号
│   │   ├── server.h               # OpenCodeServer + ACP loop + 所有子系统
│   │   └── server.cpp             # 7 端点 + run_acp_loop() + extract_tool_calls()
│   │
│   ├── llm/src/
│   │   ├── types.h                # Message (tool_call 扩展)
│   │   ├── provider.h             # LLMProvider 抽象
│   │   ├── openai_compatible_provider.h/cpp  # 通用兼容层
│   │   ├── provider_registry.h    # 多 provider 注册表
│   │   ├── tool.h                 # Tool 基类 + Permission
│   │   ├── tool_registry.h        # Tool 注册表
│   │   ├── tools/tools.h/cpp      # 6 个工具实现
│   │   ├── session_store.h/cpp    # SQLite CRUD (sessions, messages, context snapshots)
│   │   ├── context_source.h/cpp   # SystemContext + 6 内置 ContextSource
│   │   ├── client.h/cpp           # HTTPS + SSE 解析
│   │   ├── acp.h                  # ACP 协议: 事件 + SSE 帧
│   │   ├── acp_client.h/cpp       # ACP 客户端
│   │   └── log.h                  # 日志系统
│   │
│   └── util/src/config.h/cpp      # ProviderConfig + AppConfig
│
├── config/config.toml
├── bin/  (启动脚本)
└── scripts/build.sh
```

---

## SQLite 持久化

### 数据库路径

`/tmp/codis_sessions.db` (默认，可通过环境变量 `CODIS_DB_PATH` 覆盖)

### 表结构

```sql
sessions(id, created_at, updated_at, metadata)
messages(id, session_id, role, content, tool_call_id, tool_name, created_at)
context_snapshots(session_id, source_key, value_json, rendered, updated_at)
```

### SessionStore API

```cpp
class SessionStore {
    std::string create_session(metadata);
    std::optional<SessionData> load_session(id);
    std::vector<std::string> list_sessions();

    void append_message(session_id, Message);
    std::vector<Message> load_messages(session_id);

    void save_context_snapshot(session_id, key, json_value, rendered_text);
    std::optional<json> load_context_snapshot(session_id, key);
};
```

---

## System Context

### 概念模型

```
┌──────────────────────────────────────────────────────────┐
│                    System Context                         │
│  由 6 个 ContextSource 在 Context Epoch 起点组装            │
├──────────────────────────────────────────────────────────┤
│  ContextSource                                           │
│  ├─ key             稳定标识符                              │
│  ├─ loader()        可失败加载器, 返回 ContextValue          │
│  ├─ render()        首次渲染 → Baseline Text               │
│  └─ render_update() 增量渲染 → Mid-Conversation Msg        │
│                                                          │
│  Context Epoch                                           │
│  ├─ 起点: 首次 turn / compaction / provider 切换           │
│  └─ 每次 LLM 调用前 reconcile 检查变化                       │
└──────────────────────────────────────────────────────────┘
```

### 6 个内置 ContextSource

| key | 加载内容 | 渲染格式 | 增量更新 |
|-----|----------|----------|----------|
| `date` | `std::chrono::system_clock::now()` | `<current_date>2026-06-23 11:07:25</current_date>` | `The current date is now: ...` |
| `platform` | `uname()` + `$SHELL` | `<platform>OS: Linux/..., Shell: /bin/bash</platform>` | — |
| `working_dir` | `std::filesystem::current_path()` | `<working_directory>/home/user/project</working_directory>` | — |
| `git_status` | `git status --porcelain` | `<git_status>...</git_status>` | — |
| `tools` | `ToolRegistry::all_schemas()` | `<available_tools>\n- bash: ...\n- read: ...\n</available_tools>` | — |
| `project_instructions` | AGENTS.md / CONTEXT.md | `<instructions source="AGENTS.md">...</instructions>` | — |

### 工作流程

```
首次 turn:
  SystemContext::build_baseline(session_id, store)
  │
  ├─ 遍历所有 ContextSource
  │   ├─ loader() → ContextValue
  │   ├─ render() → 模型可见文本
  │   └─ store.save_context_snapshot(key, raw, rendered)
  │
  └─ 返回完整 Baseline → 作为第一个 system message

后续 turn:
  SystemContext::reconcile(session_id, store)
  │
  ├─ 遍历所有 ContextSource
  │   ├─ loader() → 新值
  │   ├─ store.load_context_snapshot(key) → 旧值
  │   └─ JSON diff
  │       ├─ 相同 → 跳过
  │       └─ 不同 → render_update() → Mid-Conversation Msg
  │
  └─ 返回增量消息 (或 nullopt)
```

---

## ACP Loop (含 System Context)

```
while (turn < 10):
    ┌─ turn == 1 → build_baseline() → system message
    └─ turn >  1 → reconcile() → 增量 system message (if changed)

    1. LLM stream (with tool schemas) → token frames
    2. extract_tool_calls(content)
       ├─ empty → break
       └─ has calls ↓
    3. for each tool_call:
       ├─ check_permission(name)
       ├─ execute()
       ├─ push tool_result frame
       └─ add to message history
    4. continue loop
```

---

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | 健康 + tools + providers |
| `GET` | `/api/v1/info` | 服务信息 |
| `POST` | `/api/v1/chat` | 非流式 (含 tools + context) |
| `POST` | `/api/v1/acp` | ACP SSE + 多轮 tool + context |
| `POST` | `/api/v1/sessions` | 创建会话 (SQLite 持久化) |
| `GET` | `/api/v1/sessions/:id` | 获取会话 + 消息历史 |
| `POST` | `/api/v1/sessions/:id/messages` | 添加消息 (持久化) |

---

## 并发模型

```
Server 进程
├─ 主线程: server.listen() 阻塞
├─ 请求线程池 (cpp-httplib 自动)
│   ├─ handle_chat()
│   └─ handle_acp() → run_acp_loop()
│       ├─ LLM stream → SseFrameQueue::push()
│       ├─ tool execution → ToolRegistry::execute()
│       ├─ SystemContext::reconcile() → ContextSnapshot diff
│       └─ SessionStore::append_message() → SQLite
│
├─ asio 信号线程
├─ ProviderRegistry → shared_mutex
├─ ToolRegistry → shared_mutex
├─ SessionStore → std::mutex
├─ SystemContext → shared_mutex
└─ Logger → std::mutex
```

---

## Phase 演进

| Phase | 版本 | 关键交付 |
|-------|------|----------|
| 1 | v0.1.0 | 单体 CLI + 非流式 LLM |
| 2 | v0.2.0 | C/S REST + Session |
| 3 | v0.3.0 | ACP + SSE 实时流式 |
| 3.1 | v0.3.1 | 多 Provider + 日志 + SSL |
| 4 | v0.4.0 | Tool Registry + 6 工具 + ACP 多轮 loop |
| 5 | v0.5.0 | **SQLite 持久化 + System Context (6 sources)** |
| 6 | v0.6.0 | TUI (FTXUI) + Anthropic Provider |
| 7 | v0.7.0 | Plugin 系统 (C ABI) |
