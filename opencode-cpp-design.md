# 基于 C++20 的 Codis AI Coding Agent 架构设计

## 项目状态

| 阶段 | 版本 | 关键交付 | 状态 |
|------|------|----------|------|
| Phase 1 | v0.1.0 | 单体 CLI + 非流式 LLM + TOML | 完成 |
| Phase 2 | v0.2.0 | C/S REST + Session | 完成 |
| Phase 3 | v0.3.0 | ACP + SSE 实时流式 | 完成 |
| Phase 4 | v0.3.1 | 多 Provider + 日志 + SSL | 完成 |
| Phase 5 | v0.4.0 | Tool Registry + 6 工具 | 完成 |
| Phase 6 | v0.5.0 | SQLite 持久化 + System Context | 完成 |
| Phase 7 | v0.6.0 | **Session 管理 (list/restore/delete) + CLI 命令** | 完成 |
| Phase 8 | v0.7.0 | ReAct + RAG | 规划中 |

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
| C++ 标准 | **C++20** | | |
| 构建系统 | **CMake** 3.20+ | | |
| 包管理 | **vcpkg** manifest (6 包) | | |
| 通信协议 | **ACP** over HTTP SSE | | |

---

## 会话管理

### 数据模型

```
sessions(id, created_at, updated_at, metadata JSON)
  metadata.title — 首条用户消息前 40 字符自动生成

messages(id, session_id FK, role, content, tool_call_id, tool_name, created_at)
```

### CLI 命令

| 命令 | 功能 |
|------|------|
| `/sessions` | 表格列出所有 session (ID / Msgs / Title) |
| `/session <id>` | 查看 session 详情 |
| `/session <id> use` | 恢复 session 到当前对话 |
| `/session <id> del` | 删除 session |
| `/clear` | 清空当前对话上下文 |

### 自动恢复

启动时自动加载 `get_last_session()`，无缝接续未完成对话。

---

## 项目目录

```
opencode-cpp/
├── CMakeLists.txt / vcpkg.json
├── ARCHITECTURE.md / opencode-cpp-design.md / plan.md
├── packages/
│   ├── cli/src/main.cpp             # CLI + /sessions /session /clear
│   ├── server/src/                  # 8 REST 端点 (含 list/delete sessions)
│   ├── llm/src/
│   │   ├── session_store.h/cpp      # SQLite CRUD + list_info / search
│   │   ├── context_source.h/cpp     # SystemContext
│   │   ├── acp_client.h/cpp         # ACP + list/delete sessions
│   │   ├── tools/                   # 6 tools
│   │   └── log.h / types.h / ...
│   └── util/src/config.h/cpp
├── config/config.toml
├── bin/ / scripts/
└── plan.md                          # ReAct + RAG 方案
```

---

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | 健康检查 |
| `POST` | `/api/v1/acp` | ACP SSE 流式 |
| `POST` | `/api/v1/chat` | 非流式聊天 |
| `POST` | `/api/v1/sessions` | 创建会话 |
| `GET` | `/api/v1/sessions` | **列出所有 (含 title/msg_count)** |
| `GET` | `/api/v1/sessions/:id` | 获取会话 + 消息 |
| `DELETE` | `/api/v1/sessions/:id` | **删除会话** |
| `POST` | `/api/v1/sessions/:id/messages` | 添加消息 |

---

## MVP 路线

| 阶段 | 版本 | 交付 |
|------|------|------|
| Phase 1 | v0.1.0 | CLI + 非流式 LLM + TOML |
| Phase 2 | v0.2.0 | C/S REST + Session |
| Phase 3 | v0.3.0 | ACP + SSE 实时流式 |
| Phase 3.1 | v0.3.1 | 多 Provider + 日志 + SSL |
| Phase 4 | v0.4.0 | Tool Registry + 6 工具 |
| Phase 5 | v0.5.0 | SQLite + System Context |
| Phase 6 | v0.6.0 | **Session 管理 + CLI 命令** |
| Phase 7 | v0.7.0 | ReAct + RAG |
| Phase 8 | v0.8.0 | Plugin 系统 (C ABI) |
