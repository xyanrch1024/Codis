# Codis AI Coding Agent 架构设计 (C++20)

## 项目状态

| Phase | 版本 | 交付 | 状态 |
|-------|------|------|------|
| 1 | v0.1.0 | CLI + 非流式 LLM | 完成 |
| 2 | v0.2.0 | C/S REST | 完成 |
| 3 | v0.3.0 | ACP + SSE | 完成 |
| 4 | v0.3.1 | 多 Provider + 日志 | 完成 |
| 5 | v0.4.0 | Tool Registry | 完成 |
| 6 | v0.5.0 | SQLite + SystemContext | 完成 |
| 7 | v0.6.0 | Session CLI | 完成 |
| 8 | v0.7.0 | Multi-client | 完成 |
| 9 | v0.8.0 | **长 TCP: WebSocket 全双工** | 完成 |
| 10 | v0.9.0 | 并发控制 + pending 排队 | 完成 |
| 11 | v0.10.0 | Plugin 系统 (C ABI) | 完成 |
| 12 | v0.11.0 | ReAct + RAG | 规划中 |

## 通信架构 (v0.8.0+)

```
1 全双工 WebSocket 长连接（发送请求 + 接收推送，同一连接）

  WS   /api/v1/acp/ws/{id}   WebSocket 全双工 (持久, JSON 帧)
       request 帧上行 → LLM；assistant/reasoning/tool_call/done 帧下行
```

| Client API | 说明 |
|------------|------|
| `connect(sid, cbs)` | 建立 WebSocket 长连接，后台线程接收所有推送，断线自动重连 |
| `send_async(req)` | 非阻塞发送 `request` 帧（走 WS）；WS 未就绪时入队，重连后补发 |
| `switch_session(sid)` | 发送 `switch` 帧（走 WS）切换 conn 到其它 session，无需 REST |

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
| 日志 | std::format + mutex | C++20 |
| 子进程 | fork/exec + pipe | POSIX |
| C++ | C++20 | |
| 构建 | CMake 3.20+ | |
| 包管理 | vcpkg manifest (6 包) | |

## 目录

```
codis-cpp/
├── CMakeLists.txt / vcpkg.json
├── ARCHITECTURE.md / codis-cpp-design.md / plan.md
├── libs/
│   ├── cli/src/main.cpp           # connect() + send_async()
│   ├── server/src/                # 2 ACP 端点 + broadcast
│   ├── llm/src/                   # Provider / Tool / Session / Log
│   └── util/src/config.h/cpp
├── config/config.toml
└── bin/ / scripts/
```

## activeSessions

```
ActiveSession { clients, processing }   ← unique_lock 保护

广播:
  LLM token → unique_lock → for client in clients:
    queue->push(frame)
  
清理: done 帧后清理过期 client
```

## MVP 路线

| Phase | 版本 | 交付 |
|------|------|------|
| 1-8 | v0.1.0-v0.8.0 | 已完成 |
| 9 | v0.9.0 | ReAct (think tool + prompt) |
| 10 | v0.10.0 | RAG (SQLite FTS5 + embedding) |
| 11 | v0.11.0 | Plugin 系统 (C ABI) |
