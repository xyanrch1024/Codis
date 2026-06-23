# 基于 C++20 的 OpenCode AI Coding Agent 架构设计

## 项目状态

| 阶段 | 版本 | 关键交付 | 状态 |
|------|------|----------|------|
| Phase 1 | v0.1.0 | 单体 CLI + 非流式 LLM + TOML | 完成 |
| Phase 2 | v0.2.0 | C/S REST + Session | 完成 |
| Phase 3 | v0.3.0 | ACP + SSE 实时流式 | 完成 |
| Phase 4 | v0.3.1 | 多 Provider + 日志 + SSL | 完成 |
| Phase 5 | v0.4.0 | Tool Registry + 6 工具 + ACP 多轮 loop | 完成 |
| Phase 6 | v0.5.0 | **SQLite 持久化 + System Context (6 sources)** | 完成 |
| Phase 7 | v0.6.0 | TUI (FTXUI) + Anthropic Provider | 规划中 |

---

## 技术选型

| 模块 | 库 | 版本 | 管理 |
|------|-----|------|------|
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
| 构建系统 | **CMake** 3.20+ | | |
| 包管理 | **vcpkg** manifest (6 包) | | |
| 通信协议 | **ACP** over HTTP SSE | | |

---

## 项目目录

```
opencode-cpp/
├── CMakeLists.txt
├── vcpkg.json                     # asio, httplib[openssl], nlohmann-json, CLI11, toml++, openssl
├── ARCHITECTURE.md                # C/S 架构详解
├── opencode-cpp-design.md         # 本文档
│
├── packages/
│   ├── cli/src/main.cpp           # CLI
│   ├── server/src/                # HTTP 守护进程
│   │   ├── main.cpp               # -p port -c config + 信号
│   │   ├── server.h               # OpenCodeServer + 所有子系统
│   │   └── server.cpp             # 7 端点 + run_acp_loop()
│   ├── llm/src/
│   │   ├── types.h                # 共享类型
│   │   ├── provider.h             # LLMProvider 抽象
│   │   ├── openai_compatible_provider.h/cpp  # 通用兼容层
│   │   ├── provider_registry.h    # 多 provider
│   │   ├── tool.h / tool_registry.h / tools/  # Tool 系统
│   │   ├── session_store.h/cpp    # SQLite CRUD
│   │   ├── context_source.h/cpp   # SystemContext + 6 sources
│   │   ├── client.h/cpp           # HTTPS + SSE
│   │   ├── acp.h / acp_client.h/cpp  # ACP 协议
│   │   └── log.h                  # 日志
│   └── util/src/config.h/cpp      # 配置
│
├── config/config.toml
├── bin/
└── scripts/build.sh
```

---

## SQLite 持久化

### SessionStore

```cpp
class SessionStore {
    // 会话: create / load / delete / list
    // 消息: append_message / load_messages
    // Context 快照: save_context_snapshot / load_context_snapshot
};
```

数据库: `/tmp/codis_sessions.db`, WAL 模式, FOREIGN KEYS

### 表

```
sessions(id PK, created_at, updated_at, metadata)
messages(id PK, session_id FK, role, content, tool_call_id, tool_name)
context_snapshots(session_id, source_key PK, value_json, rendered)
```

---

## System Context

### ContextSource

```cpp
struct ContextSource {
    std::string key;
    Loader loader;        // → ContextValue (raw JSON + rendered text)
    Renderer render;      // → baseline text
    optional<Renderer> render_update;  // → incremental update text
};
```

### 6 个内置源

| key | 加载内容 | 增量 |
|-----|----------|------|
| `date` | 当前时间 | `The current date is now: ...` |
| `platform` | OS + Shell | — |
| `working_dir` | 工作目录 | — |
| `git_status` | `git status --porcelain` | — |
| `tools` | ToolRegistry schemas | — |
| `project_instructions` | AGENTS.md / CONTEXT.md | — |

### 工作流程

```
build_baseline(session_id, store):
  for each source:
    val = loader()
    text = render(val)
    store.save_context_snapshot(key, val.raw, text)
  return combined text → first system message

reconcile(session_id, store):
  for each source:
    val = loader()
    old = store.load_context_snapshot(key)
    if val.raw == old: continue
    update = render_update(val) or render(val)
    store.save_context_snapshot(key, val.raw, text)
  return combined update → mid-conversation system message
```

---

## ACP Loop (含 System Context)

```
while turn < 10:
    turn == 1 → build_baseline() → system message
    turn >  1 → reconcile() → system message (if changed)

    1. LLM stream (with tools) → assistant frames
    2. extract_tool_calls(content)
       ├─ empty → break
       └─ has calls → execute each tool → tool_result frames
    3. add tool results to message history
    4. continue
```

---

## MVP 路线

| 阶段 | 版本 | 交付 |
|------|------|------|
| Phase 1 | v0.1.0 | CLI + 非流式 LLM + TOML |
| Phase 2 | v0.2.0 | C/S 架构 + REST API + Session |
| Phase 3 | v0.3.0 | ACP + SSE 实时流式 |
| Phase 3.1 | v0.3.1 | 多 Provider + 日志 + SSL 修复 |
| Phase 4 | v0.4.0 | Tool Registry + 6 工具 + ACP 多轮 loop |
| Phase 5 | v0.5.0 | **SQLite 持久化 + System Context** |
| Phase 6 | v0.6.0 | TUI (FTXUI) + Anthropic |
| Phase 7 | v0.7.0 | Plugin 系统 (C ABI) |
