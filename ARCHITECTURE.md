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
└──────────────┘                      │  ├─ SseFrameQueue        │
                                      │  ├─ SessionManager        │
                                      │  └─ Logger (log.h)        │
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
| 日志 | **std::format + std::mutex** | C++20 | 标准库 (log.h) |
| 构建 | **CMake** 3.20+ | | |
| 包管理 | **vcpkg** manifest (6 包) | | |
| C++ 标准 | **C++20** | | |
| 通信协议 | **ACP** over HTTP SSE | | |

---

## 项目目录结构

```
opencode-cpp/
├── CMakeLists.txt                 # 6 个 find_package，零 FetchContent
├── vcpkg.json                     # asio, httplib[openssl], nlohmann-json, CLI11, toml++, openssl
├── ARCHITECTURE.md                # 本文档
│
├── packages/
│   ├── cli/src/main.cpp           # CLI → AcpClient → 事件回调 + log
│   │
│   ├── server/src/                # HTTP 守护进程 (多 provider)
│   │   ├── main.cpp               # -p port -c config.toml + 信号 + log
│   │   ├── server.h               # OpenCodeServer + SessionManager + SseFrameQueue + ProviderRegistry
│   │   └── server.cpp             # 7 个 REST 端点 + resolve_provider() + log
│   │
│   ├── llm/src/                   # 共享库
│   │   ├── types.h                # Message, ChatRequest, ChatResponse
│   │   ├── provider.h             # LLMProvider 抽象接口
│   │   ├── openai_compatible_provider.h/cpp  # 通用 OpenAI 兼容层
│   │   ├── provider_registry.h    # 多 provider 注册表 (shared_mutex)
│   │   ├── client.h/cpp           # 原始 HTTPS 客户端 (HTTPS + SSE 解析)
│   │   ├── acp.h                  # ACP 协议: 5种事件 + SSE 帧序列化
│   │   ├── acp_client.h/cpp       # ACP 客户端 (SSE 解析 + 事件回调)
│   │   └── log.h                  # 日志系统 (5 级, 环境变量, 文件输出)
│   │
│   └── util/src/config.h/cpp      # ProviderConfig (api_key_env) + AppConfig
│
├── config/config.toml             # 多 provider 配置
├── bin/                           # 启动脚本
└── scripts/build.sh
```

---

## 日志系统

### 架构

```
log.h (header-only, C++20, 零依赖)
│
├─ Logger (单例, std::mutex 线程安全)
│   ├─ Level: trace / debug / info / warn / error / off
│   ├─ 输出: stderr + 可选文件
│   └─ 格式: [HH:MM:SS.ms] [LEVEL] [tID] file:line  msg
│
└─ 5 个宏: LOG_TRACE / LOG_DEBUG / LOG_INFO / LOG_WARN / LOG_ERROR
```

### 环境变量

```bash
OPENCODE_LOG_LEVEL=trace|debug|info|warn|error|off
OPENCODE_LOG_FILE=/path/to/logfile
```

### 编译期裁剪

```cmake
# CMakeLists.txt — Release 构建裁剪 TRACE
add_compile_definitions(LOG_ACTIVE_LEVEL=2)  # 只保留 INFO+
```

### 日志覆盖

| 模块 | 关键日志 |
|------|----------|
| server 启动/停止 | INFO server listening / stopped |
| provider 注册 | INFO provider 'deepseek' registered |
| provider 解析 | DEBUG resolving provider 'deepseek' |
| LLM 调用 | DEBUG deepseek::stream_chat model=... |
| HTTP 请求 | DEBUG POST https://api.deepseek.com/... |
| HTTP 状态 | WARN HTTP 302 / ERROR HTTP failed |
| SSE 解析 | TRACE SSE response N bytes, parsing... |
| ACP 客户端 | DEBUG ACP POST provider=... |
| 信号处理 | INFO received shutdown signal |

---

## Provider 架构

```
LLMProvider (abstract interface)
│
└── OpenAICompatibleProvider        ← 通用实现，覆盖所有 OpenAI 兼容 API
        │                             零代码新增 provider — 只需配置
        │
        ├── OpenAI    → https://api.openai.com/v1
        ├── DeepSeek  → https://api.deepseek.com/v1
        ├── Groq      → https://api.groq.com/openai/v1
        ├── Together  → https://api.together.xyz/v1
        └── ...

ProviderRegistry (server 端, shared_mutex)
├── register_provider(ProviderConfig)
├── get(name) → optional<shared_ptr<LLMProvider>>
├── list() → vector<string>
└── default_name() → string
```

---

## ACP 协议

### 事件类型

| Event | SSE 帧 | 说明 |
|-------|--------|------|
| `assistant` | `data: {"type":"assistant","data":{"delta":"..."}}` | LLM 增量 token |
| `tool_call` | `data: {"type":"tool_call","data":{"id":"...","name":"bash",...}}` | 请求工具调用 |
| `tool_result` | `data: {"type":"tool_result","data":{"id":"...","success":true,...}}` | 工具结果 |
| `error` | `data: {"type":"error","data":{"message":"..."}}` | 错误 |
| `done` | `data: {"type":"done","data":{}}` | 对话完成 |

### 端到端数据流

```
Client                            Server
  │                                 │
  │ POST /api/v1/acp                │
  │ ChatRequest(JSON)               │
  │ ────────────────────────────►  │
  │                                 │────► LLMHttpClient::stream_post()
  │                                 │      POST https://api.deepseek.com/v1/chat/completions
  │                                 │      stream=true
  │                                 │      ◄── SSE body
  │                                 │      逐行解析 → on_token(delta)
  │                                 │           │
  │                                 │     OpenAICompatibleProvider::stream_chat()
  │                                 │           │
  │                                 │     call_llm_stream() → SseFrameQueue::push()
  │                                 │           │
  │  ◄── chunked_content_provider ────────── pop() → sink.write()
  │  data: {"type":"assistant",     │
  │         "data":{"delta":"Hi"}}  │
  │  data: {"type":"assistant",     │
  │         "data":{"delta":"!"}}   │
  │  data: {"type":"done"}          │
```

---

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | `{"status":"ok","version":"0.3.1","default_provider":"deepseek"}` |
| `GET` | `/api/v1/info` | `{"providers":["deepseek","openai"],"features":["acp",...]}` |
| `POST` | `/api/v1/chat` | 非流式聊天 → JSON |
| `POST` | `/api/v1/acp` | ACP SSE 流式 → `text/event-stream` |
| `POST` | `/api/v1/sessions` | 创建会话 → `201` |
| `GET` | `/api/v1/sessions/:id` | 获取会话 |
| `POST` | `/api/v1/sessions/:id/messages` | 添加消息 |

---

## 并发模型

```
Server 进程
├─ 主线程: server.listen() 阻塞
├─ 请求线程池 (cpp-httplib 自动)
│   ├─ handle_chat() → resolve_provider() → chat()
│   └─ handle_acp()  → detach LLM worker + chunked stream
├─ LLM worker 线程 → stream_post() → SSE 解析 → push frames
├─ asio 信号线程 → SIGINT/SIGTERM
├─ ProviderRegistry → shared_mutex
├─ SessionManager → shared_mutex
└─ Logger → std::mutex
```

---

## 配置示例

```toml
# config.toml
default_provider = "deepseek"

[llm]
max_tokens = 4096
temperature = 0.7

[[providers]]
name = "openai"
api_key_env = "OPENAI_API_KEY"
model = "gpt-4o"
base_url = "https://api.openai.com/v1"

[[providers]]
name = "deepseek"
api_key_env = "DEEPSEEK_API_KEY"
model = "deepseek-chat"
base_url = "https://api.deepseek.com/v1"
```

---

## Phase 演进

| Phase | 版本 | 关键交付 |
|-------|------|----------|
| 1 | v0.1.0 | 单体 CLI + 非流式 LLM + TOML |
| 2 | v0.2.0 | C/S REST + Session |
| 3 | v0.3.0 | ACP + SSE 实时流式 |
| 4 | v0.3.1 | **多 Provider** (OpenAI/DeepSeek) + **日志系统** + **SSL 修复** |
| 5 | v0.4.0 | Tool Registry + 工具执行 |

---

## 已修复问题 (v0.3.1)

| 问题 | 根因 | 修复 |
|------|------|------|
| CLI 无响应 / SSE 不工作 | `LLMHttpClient` 流式路径是 TODO 空壳 | 实现 SSE 逐行解析 |
| HTTPS 请求失败 (302) | `Client(host)` 不带 scheme | `Client("https://"+host)` |
| `https` scheme 不支持 | cpp-httplib 默认无 OpenSSL | `vcpkg.json`: `httplib[openssl]` |
