# Codis AI Coding Agent 架构设计 (C++20)

## 项目状态

| 阶段 | 版本 | 交付 | 状态 |
|------|------|------|------|
| Phase 1 | v0.1.0 | CLI + 非流式 LLM | 完成 |
| Phase 2 | v0.2.0 | C/S REST + Session | 完成 |
| Phase 3 | v0.3.0 | ACP + SSE 流式 | 完成 |
| Phase 4 | v0.3.1 | 多 Provider + 日志 | 完成 |
| Phase 5 | v0.4.0 | Tool Registry + 6 工具 | 完成 |
| Phase 6 | v0.5.0 | SQLite + System Context | 完成 |
| Phase 7 | v0.6.0 | Session CLI 管理 | 完成 |
| Phase 8 | v0.7.0 | **activeSessions 多 client 共享 + 广播** | 完成 |
| Phase 9 | v0.8.0 | ReAct + RAG | 规划中 |

## 技术选型

| 模块 | 库 | 版本 |
|------|-----|------|
| HTTP 客户端/服务端 | cpp-httplib | 0.47.0 [openssl] |
| JSON | nlohmann/json | 3.12.0 |
| CLI | CLI11 | 2.6.2 |
| 配置 | toml++ | 3.4.0 |
| SSL | OpenSSL | 3.6.3 |
| 异步 IO | standalone asio | 1.32.0 |
| 数据库 | SQLite3 | 3.45.1 |
| C++ 标准 | C++20 | |
| 构建 | CMake 3.20+ | |
| 包管理 | vcpkg manifest (6 包) | |

## 核心架构

### activeSessions

```cpp
// 服务端内存表：session_id → 活跃状态
map<string, ActiveSession> active_sessions_;  // shared_mutex

struct ActiveSession {
    vector<weak_ptr<SseFrameQueue>> clients;  // 所有监听者
    atomic<bool> processing;                    // LLM 是否在运行
};
```

### attach 逻辑

```
attach_to_session(session_id, req, is_new):
  1. 检查 session 是否存在/processing
  2. 从 SessionStore 加载历史 → push 到 client queue
  3. 判断:
     a. processing=true → 只加入监听 (不触发 LLM)
     b. messages 无 user 内容 → 只同步历史 (不触发 LLM)
     c. 有新消息 → 加入监听 + 启动 LLM + 广播
```

### 广播

```cpp
run_acp_loop_broadcast(session_id, req):
  stream_chat → broadcast(assistant_frame)
  tool.execute → broadcast(tool_result_frame)
  done → broadcast(done_frame) → erase from active_sessions
```

## CLI

### 启动体验

```bash
./opencode -i                    # 自动恢复最后 session，显示历史
./opencode -i -S <session_id>   # attach 到指定 session
```

```
╔══════════════════════════════════════════╗
║  Codis Client  v0.7.0                  ║
║  Server:   localhost:8711               ║
║  Session:  2dd48b8c...                  ║
╚══════════════════════════════════════════╝
── 3 messages loaded from session ──
You: 用C++写hello world
AI: 以下是...
──────────────────────────────────────
Commands: /exit /sessions /session <id> /clear
>
```

### 命令

| 命令 | 功能 |
|------|------|
| `/sessions` | 表格列出所有 |
| `/session <id>` | 切换 + 显示历史 |
| `/session del <id>` | 删除 |
| `/clear` | 清空 |

## 配置（仅环境变量）

```toml
[[providers]]
name = "deepseek"
api_key_env = "DEEPSEEK_API_KEY"
model = "deepseek-chat"
base_url = "https://api.deepseek.com"
```

```bash
export DEEPSEEK_API_KEY="sk-..."
./opencode-server -p 8711 -c config/config.toml
```

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | 健康 |
| `POST` | `/api/v1/acp` | ACP SSE (支持 session_id) |
| `GET` | `/api/v1/sessions` | 列出 |
| `GET` | `/api/v1/sessions/:id` | 详情 + 消息 |
| `DELETE` | `/api/v1/sessions/:id` | 删除 |
| `POST` | `/api/v1/sessions/:id/messages` | 添加 |

## MVP 路线

| Phase | 版本 | 交付 |
|------|------|------|
| 1 | v0.1.0 | CLI + 非流式 LLM |
| 2 | v0.2.0 | C/S REST + Session |
| 3 | v0.3.0 | ACP + SSE |
| 4 | v0.3.1 | 多 Provider + 日志 |
| 5 | v0.4.0 | Tool Registry |
| 6 | v0.5.0 | SQLite + SystemContext |
| 7 | v0.6.0 | CLI Session 命令 |
| 8 | v0.7.0 | **activeSessions 多 client 共享** |
| 9 | v0.8.0 | ReAct + RAG |
