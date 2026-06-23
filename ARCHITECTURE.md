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
                                      │  │   ├─ bash (shell)     │
                                      │  │   ├─ read (文件读取)    │
                                      │  │   ├─ write (文件写入)   │
                                      │  │   ├─ edit (字符串替换)   │
                                      │  │   ├─ glob (模式匹配)    │
                                      │  │   └─ grep (正则搜索)    │
                                      │  ├─ ACP Loop (多轮工具)    │
                                      │  ├─ SseFrameQueue         │
                                      │  ├─ SessionManager         │
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
| 日志 | **std::format** + std::mutex | C++20 | 标准库 |
| 子进程 | **fork/exec + pipe** | POSIX | 系统调用 |
| 文件操作 | **std::filesystem / std::regex** | C++17 | 标准库 |
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
│   ├── server/src/                # HTTP 守护进程 (多 provider + 多工具)
│   │   ├── main.cpp               # -p port -c config.toml + 信号
│   │   ├── server.h               # OpenCodeServer + ACP loop + ProviderRegistry + ToolRegistry
│   │   └── server.cpp             # 7 端点 + run_acp_loop() + extract_tool_calls()
│   │
│   ├── llm/src/
│   │   ├── types.h                # Message (含 tool_call_id/name), ChatRequest/Response
│   │   ├── provider.h             # LLMProvider 抽象
│   │   ├── openai_compatible_provider.h/cpp  # 通用兼容层
│   │   ├── provider_registry.h    # 多 provider 注册表
│   │   ├── tool.h                 # Tool 基类 + Permission + ToolSchema/Call/Result
│   │   ├── tool_registry.h        # Tool 注册表 (shared_mutex)
│   │   ├── tools/tools.h          # 6 个工具声明
│   │   ├── tools/tools.cpp        # 6 个工具实现
│   │   ├── client.h/cpp           # HTTPS 客户端 + SSE 解析
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

## Tool Registry 架构

```
Tool (抽象基类)
├── schema() → ToolSchema            JSON Schema for LLM function calling
├── default_permission() → Permission  Deny / Ask / Allow
├── execute(ToolCall) → ToolResult   同步执行

ToolRegistry (线程安全)
├── register_tool(unique_ptr<Tool>)
├── all_schemas() → vector<ToolSchema>
├── execute(ToolCall) → ToolResult
├── check_permission(name) → Permission
└── list() → vector<string>
```

### 6 个内置工具

| Tool | Permission | 功能 | 实现 |
|------|-----------|------|------|
| `bash` | Ask | 执行 shell 命令 | fork+exec, 30s 超时, stdout+stderr 捕获, 64KB 截断 |
| `read` | Allow | 读取文件 | std::ifstream, offset/limit 分页, 行号前缀 |
| `write` | Allow | 写入文件 | std::ofstream, 自动创建父目录 |
| `edit` | Ask | 字符串替换 | 读文件→find→replace→写回, replaceAll, 自动 .bak |
| `glob` | Allow | 文件模式匹配 | recursive_directory_iterator, `**/*.cpp` 风格 |
| `grep` | Allow | 正则内容搜索 | std::regex, 文件/目录递归, 扩展名过滤, 200 匹配上限 |

---

## ACP Loop (多轮工具调用)

```
Client POST /api/v1/acp
      │
      ▼
handle_acp()
      │
      ▼  ┌──────────────────────────────────────┐
      └─►│         ACP Loop (max 10 turns)       │
         │                                       │
         │  1. build LLM request + tool schemas  │
         │  2. call_llm_stream() → token frames  │
         │  3. extract_tool_calls()              │
         │     ├─ no calls → break, done         │
         │     └─ has calls ↓                    │
         │  4. for each tool_call:               │
         │     ├─ check_permission(name)         │
         │     │   ├─ Denied → error frame        │
         │     │   └─ Allow/Ask → execute()      │
         │     ├─ push tool_result_frame()       │
         │     └─ add to message history          │
         │  5. goto 1 (LLM continues)             │
         └───────────────────────────────────────┘
```

---

## ACP 协议

### 事件类型

| Event | SSE 帧 | 说明 |
|-------|--------|------|
| `assistant` | `data: {"type":"assistant","data":{"delta":"..."}}` | LLM 增量 token |
| `tool_call` | `data: {"type":"tool_call","data":{"id":"...","name":"bash","arguments":{...}}}` | 请求工具调用 |
| `tool_result` | `data: {"type":"tool_result","data":{"id":"...","success":true,"content":"..."}}` | 工具结果 |
| `error` | `data: {"type":"error","data":{"message":"..."}}` | 错误 |
| `done` | `data: {"type":"done","data":{}}` | 对话完成 |

---

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | `{"status":"ok","version":"0.4.0","tools":["bash","read",...],"default_provider":"deepseek"}` |
| `GET` | `/api/v1/info` | `{"providers":[...],"tools":[...],"features":["acp","chat","tools",...]}` |
| `POST` | `/api/v1/chat` | 非流式聊天 (含 tools) |
| `POST` | `/api/v1/acp` | ACP SSE 流式 + 多轮 tool call |
| `POST` | `/api/v1/sessions` | 创建会话 |
| `GET` | `/api/v1/sessions/:id` | 获取会话 |
| `POST` | `/api/v1/sessions/:id/messages` | 添加消息 |

---

## 并发模型

```
Server 进程
├─ 主线程: server.listen() 阻塞
├─ 请求线程池 (cpp-httplib 自动)
│   ├─ handle_chat() → call_llm()
│   └─ handle_acp()  → detach LLM worker + run_acp_loop()
│       │
│       ├─ LLM stream → SseFrameQueue::push()
│       └─ tool execution → ToolRegistry::execute()
│
├─ asio 信号线程 → SIGINT/SIGTERM
├─ ProviderRegistry → shared_mutex
├─ ToolRegistry → shared_mutex
├─ SessionManager → shared_mutex
└─ Logger → std::mutex
```

---

## 配置

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
| 1 | v0.1.0 | 单体 CLI + 非流式 LLM |
| 2 | v0.2.0 | C/S REST + Session |
| 3 | v0.3.0 | ACP + SSE 实时流式 |
| 3.1 | v0.3.1 | 多 Provider + 日志系统 + SSL 修复 |
| 4 | v0.4.0 | **Tool Registry + 6 个工具 + ACP 多轮 loop** |
| 5 | v0.5.0 | TUI (FTXUI) + Anthropic Provider |
| 6 | v0.6.0 | SQLite 持久化 + System Context |
| 7 | v0.7.0 | Plugin 系统 (C ABI) |
