# Codis — C++ AI Coding Agent

A C++20 AI coding assistant with multi-provider support, multi-client shared sessions, Feishu bot, and a TUI interface.

> 🇨🇳 [中文 README](./README.zh.md)

## Quick Start (Docker)

```bash
docker build -t codis .
docker run -d --name codis \
  -e FEISHU_APP_ID="cli_xxx" \
  -e FEISHU_APP_SECRET="xxx" \
  -e GLM_API_KEY="xxx" \
  -e LOG_LEVEL=info \
  -p 8711:8711 \
  codis
docker logs -f codis
```

## Architecture

```
┌──────────────┐  fire-and-forget (REST)  ┌─────────────────────────────────┐
│  codis        │ ◄─────────────────────► │  codis-server                   │
│  (CLI/TUI)   │  SSE long-lived stream   │  (daemon)                       │
│              │                          │                                 │
│  send()      │  POST /api/v1/acp        │  ├─ SessionState (per session)  │
│  connect()   │  ──────────────────────► │  │   conn_id → queue broadcast  │
│              │  GET /api/v1/acp/stream   │  ├─ ProviderRegistry           │
│  commands:    │ ◄══ SSE keep-alive ════ │  │   OpenAI/DeepSeek/GLM       │
│  /sessions   │                          │  ├─ ToolRegistry (6)           │
│  /session id │                          │  ├─ SystemContext (6)          │
│  /clear      │                          │  ├─ SessionStore (SQLite)      │
│  /clearsessions                         │  └─ Logger                     │
└──────────────┘                          └─────────────────────────────────┘
                                                   ▲
                                                   │ HTTP REST
                                                   │
┌──────────────────┐                              │
│  Python Bot       │◄─────────────────────────────┘
│                   │
│  feishu_bot.py    │── WebSocket ──► Feishu Server
│                   │
│  80 lines Python  │  lark-oapi SDK
└──────────────────┘
```

## Features

- **C/S Architecture** — Server daemon + CLI / Python Bot clients
- **Multi-Provider** — OpenAI / DeepSeek / GLM / Groq, config-driven
- **SessionState** — Per-session connection queues with conn_id routing
- **Long-lived TCP** — SSE stream keepalive + fire-and-forget ACP
- **Multi-Client** — Independent channels per session, no cross-talk
- **Tool Registry** — bash, read, write, edit, glob, grep
- **System Context** — date, platform, git_status, AGENTS.md
- **SQLite Persistence** — Sessions, messages, context snapshots
- **Session Management** — list / restore / delete / search
- **Plugin System** — C ABI dlopen, dynamic tool loading
- **Logging** — 5 levels, env-var controlled
- **Feishu Bot** — Python lark-oapi SDK, WebSocket, no public IP needed
- **FTXUI TUI** — Terminal UI with conversation view, input bar, live SSE updates
- **Docker** — One-container deployment, zero manual config

## CLI Commands

| Command | Description |
|------|------|
| `/sessions` | List all sessions in a table |
| `/session <id> use` | Resume a session |
| `/session <id> del` | Delete a session |
| `/clear` | Clear current context |
| `/clearsessions` | Delete all sessions |

## Configuration

```toml
default_provider = "glm"

[[providers]]
name = "glm"
api_key_env = "GLM_API_KEY"
model = "glm-4.5-flash"
base_url = "https://open.bigmodel.cn/api/paas/v4"
```

API keys are set via environment variables — never in the config file.

## REST API

| Method | Path | Description |
|------|------|------|
| `GET` | `/api/v1/health` | Health check |
| `POST` | `/api/v1/chat` | Synchronous chat |
| `POST` | `/api/v1/acp` | Fire-and-forget with conn_id |
| `GET` | `/api/v1/acp/stream/{id}` | SSE long-lived stream |
| `GET` | `/api/v1/sessions` | List sessions |
| `DELETE` | `/api/v1/sessions` | Delete all sessions |
| `DELETE` | `/api/v1/sessions/:id` | Delete a session |

## Tech Stack

| Category | Library | Version |
|------|------|------|
| HTTP Server | cpp-httplib | 0.47.0 (OpenSSL) |
| JSON | nlohmann/json | 3.12.0 |
| CLI Parsing | CLI11 | 2.6.2 |
| Config | toml++ | 3.4.0 |
| SSL | OpenSSL | 3.6.3 |
| Async I/O | standalone asio | 1.32.0 |
| Database | SQLite3 | 3.45.1 |
| TUI | FTXUI | 7.0.0 |
| Feishu SDK | lark-oapi | (Python) |
| Build | CMake 3.20+ / vcpkg | |
| Language | C++20 | |
