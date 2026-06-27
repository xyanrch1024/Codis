# Codis — C++ AI 编程助手

基于 C++20 的 AI 编程助手，支持多 Provider、多 Client 共享 Session、飞书 Bot 接入和 TUI 界面。

> 🇺🇸 [English README](./README.md)

## 快速开始 (Docker)

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

## 快速开始 (原生)

### 编译

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j$(nproc)
```

### 运行

```bash
# 终端 1: 启动 server
export GLM_API_KEY="your-api-key"
./build/packages/server/opencode-server -c config/config.toml

# 终端 2: 交互 CLI
./build/packages/cli/opencode -i

# 或启动 TUI
./build/packages/cli/opencode --tui

# 单次查询
./build/packages/cli/opencode "什么是C++20?"
```

## 架构

```
┌──────────────┐  fire-and-forget (REST)  ┌─────────────────────────────────┐
│  codis        │ ◄─────────────────────► │  codis-server                   │
│  (CLI/TUI)   │  SSE long-lived stream   │  (后台守护进程)                  │
│              │                          │                                 │
│  send()      │  POST /api/v1/acp        │  ├─ SessionState (per session)  │
│  connect()   │  ──────────────────────► │  │   conn_id → queue 直接广播    │
│              │  GET /api/v1/acp/stream   │  ├─ ProviderRegistry           │
│  交互命令:    │ ◄══ SSE keep-alive ════ │  │   OpenAI/DeepSeek/GLM        │
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
│  feishu_bot.py    │── WebSocket ──► 飞书服务器
│                   │
│  80 行 Python     │  lark-oapi SDK
└──────────────────┘
```

## 特性

- **C/S 架构** — Server 守护进程 + CLI / Python Bot 客户端
- **多 Provider** — OpenAI / DeepSeek / GLM / Groq，配置驱动
- **SessionState 广播** — 每 session 独立管理连接队列，conn_id 精准路由
- **长 TCP 连接** — SSE stream keepalive + fire-and-forget ACP
- **多 Client 共享** — 同 session 多客户端独立通道，无交叉干扰
- **Tool Registry** — bash, read, write, edit, glob, grep
- **System Context** — date, platform, git_status, AGENTS.md
- **SQLite 持久化** — 会话/消息/Context 快照
- **Session 管理** — list / restore / delete / search
- **Plugin 系统** — C ABI dlopen，动态加载自定义工具
- **日志系统** — 5 级，环境变量控制
- **飞书 Bot** — Python lark-oapi SDK，WebSocket 长连接，无需公网 IP
- **FTXUI TUI** — 终端界面，对话视图 + 输入栏 + SSE 实时推送
- **Docker 一键部署** — 单容器运行，零手动配置

## CLI 命令

| 命令 | 功能 |
|------|------|
| `/sessions` | 表格列出所有 session |
| `/session <id> use` | 恢复会话 |
| `/session <id> del` | 删除会话 |
| `/clear` | 清空当前上下文 |
| `/clearsessions` | 删除所有会话 |

## 配置

```toml
default_provider = "glm"

[[providers]]
name = "glm"
api_key_env = "GLM_API_KEY"
model = "glm-4.5-flash"
base_url = "https://open.bigmodel.cn/api/paas/v4"
```

API Key 通过环境变量设置，不在配置文件中写明文。

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | 健康检查 |
| `POST` | `/api/v1/chat` | 同步聊天 |
| `POST` | `/api/v1/acp` | fire-and-forget 带 conn_id |
| `GET` | `/api/v1/acp/stream/{id}` | SSE 长连接 |
| `GET` | `/api/v1/sessions` | 列出会话 |
| `DELETE` | `/api/v1/sessions` | 删除所有会话 |
| `DELETE` | `/api/v1/sessions/:id` | 删除会话 |

## 技术栈

| 模块 | 库 | 版本 |
|------|-----|------|
| HTTP | cpp-httplib | 0.47.0 (OpenSSL) |
| JSON | nlohmann/json | 3.12.0 |
| CLI | CLI11 | 2.6.2 |
| 配置 | toml++ | 3.4.0 |
| SSL | OpenSSL | 3.6.3 |
| 异步 IO | standalone asio | 1.32.0 |
| 数据库 | SQLite3 | 3.45.1 |
| TUI | FTXUI | 7.0.0 |
| 飞书 SDK | lark-oapi | (Python) |
| 构建 | CMake 3.20+ / vcpkg | |
| 语言 | C++20 | |
