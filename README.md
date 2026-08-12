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
  -e OPENCODE_LOG_LEVEL=info \
  -p 8711:8711 \
  codis
docker logs -f codis
```

## Quick Start (Native)

### Build

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j$(nproc)
```

### Run

```bash
# Terminal 1: start server
export GLM_API_KEY="your-api-key"
./build/packages/server/opencode-server -c config/config.toml

# Terminal 2: interactive CLI (default)
./build/packages/cli/opencode

# Or launch the TUI
./build/packages/cli/opencode --tui

# Continue the last session
./build/packages/cli/opencode --tui -c
```

### Logging

Logs are controlled by environment variables:

| Env | Default | Description |
|------|---------|------|
| `OPENCODE_LOG_LEVEL` | `info` | `trace` / `debug` / `info` / `warn` / `error` / `off` |
| `OPENCODE_LOG_FILE` | unset | When set, logs go to the file only (keeps the fullscreen TUI clean); otherwise they go to stderr |

## Architecture

```
┌──────────────┐  fire-and-forget (REST)  ┌─────────────────────────────────┐
│  codis        │ ◄─────────────────────► │  codis-server                   │
│  (CLI/TUI)   │  WebSocket push          │  (daemon)                       │
│              │                          │                                 │
│  send_async()│  POST /api/v1/acp        │  ├─ SessionState (per session)  │
│  connect()   │  ──────────────────────► │  │   conn_id → queue broadcast  │
│              │  WS /api/v1/acp/ws/{id}  │  ├─ ProviderRegistry           │
│  commands:    │ ◄══ keep-alive WS ═════ │  │   OpenAI/DeepSeek/GLM       │
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
- **Long-lived TCP** — WebSocket push + fire-and-forget ACP
- **Multi-Client** — Independent channels per session, no cross-talk
- **In-flight queueing** — Messages arriving during LLM processing are queued and re-run after the current turn (no silent drops)
- **Tool Registry** — bash, read, write, edit, glob, grep
- **System Context** — date, platform, git_status, AGENTS.md
- **SQLite Persistence** — Sessions, messages, context snapshots
- **Session Management** — list / restore / delete / search
- **Plugin System** — C ABI dlopen, dynamic tool loading
- **Logging** — 5 levels, env-var controlled
- **Feishu Bot** — Python lark-oapi SDK, WebSocket, no public IP needed
- **FTXUI TUI** — Terminal UI with color-coded messages, input bar, live WS updates
- **Docker** — One-container deployment, zero manual config

## CLI Commands

| Command | Description |
|------|------|
| `/sessions` | List all sessions in a table |
| `/session <id> use` | Resume a session |
| `/session <id> del` | Delete a session |
| `/newsession` | Create a new session (TUI) |
| `/clear` | Clear current context |
| `/clearsessions` | Delete all sessions |
| `/balance [provider]` | Query provider account balance |

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
| `GET` | `/api/v1/info` | Server info (providers, tools, version) |
| `POST` | `/api/v1/chat` | Synchronous chat |
| `POST` | `/api/v1/acp` | Fire-and-forget with conn_id |
| `POST` | `/api/v1/acp/switch` | Move a conn_id to another session |
| `WS` | `/api/v1/acp/ws/{id}` | Long-lived WebSocket push (JSON frames) |
| `POST` | `/api/v1/sessions` | Create a session |
| `GET` | `/api/v1/sessions` | List sessions |
| `GET` | `/api/v1/sessions/:id` | Get a session with messages |
| `DELETE` | `/api/v1/sessions` | Delete all sessions |
| `DELETE` | `/api/v1/sessions/:id` | Delete a session |
| `POST` | `/api/v1/sessions/:id/messages` | Append a message |
| `GET` | `/api/v1/balance/:provider` | Provider account balance |

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
