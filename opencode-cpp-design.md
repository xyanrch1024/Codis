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
| 9 | v0.8.0 | **长 TCP: SSE stream + fire-and-forget** | 完成 |
| 10 | v0.9.0 | ReAct + RAG | 规划中 |

## 通信架构 (v0.8.0)

```
1 SSE 长连接 + N REST fire-and-forget

  GET  /api/v1/acp/stream/{id}   SSE 长连接 (持久)
  POST /api/v1/acp               fire-and-forget (202)
```

| Client API | 说明 |
|------------|------|
| `connect(sid, cbs)` | 建立 SSE 长连接，后台线程接收所有推送 |
| `send_async(req)` | 非阻塞发送消息，回复通过 SSE stream 到达 |

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
opencode-cpp/
├── CMakeLists.txt / vcpkg.json
├── ARCHITECTURE.md / opencode-cpp-design.md / plan.md
├── packages/
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
