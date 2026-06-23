# 基于 C++20 的 OpenCode AI Coding Agent 架构设计

## 项目状态

| 阶段 | 版本 | 关键交付 | 状态 |
|------|------|----------|------|
| Phase 1 | v0.1.0 | 单体 CLI + 非流式 LLM + TOML | 完成 |
| Phase 2 | v0.2.0 | C/S REST + Session | 完成 |
| Phase 3 | v0.3.0 | ACP + SSE 实时流式 + 事件回调 | 完成 |
| Phase 4 | v0.3.1 | 多 Provider + 日志系统 + SSL 修复 | 完成 |
| Phase 5 | v0.4.0 | **Tool Registry + 6 工具 + ACP 多轮 loop** | 完成 |
| Phase 6 | v0.5.0 | TUI (FTXUI) + Anthropic Provider | 规划中 |

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
│   ├── cli/src/main.cpp           # CLI → AcpClient → 事件回调
│   ├── server/src/                # HTTP 守护进程
│   │   ├── main.cpp               # -p port -c config + 信号
│   │   ├── server.h               # OpenCodeServer + ACP loop + ProviderRegistry + ToolRegistry
│   │   └── server.cpp             # 7 端点 + run_acp_loop() + extract_tool_calls()
│   ├── llm/src/
│   │   ├── types.h                # Message (tool_call 扩展), ChatRequest/Response
│   │   ├── provider.h             # LLMProvider 抽象
│   │   ├── openai_compatible_provider.h/cpp  # 通用兼容层
│   │   ├── provider_registry.h    # 多 provider
│   │   ├── tool.h                 # Tool 基类 + Permission + Schema/Call/Result
│   │   ├── tool_registry.h        # Tool 注册表 (shared_mutex)
│   │   ├── tools/tools.h/cpp      # 6 工具: bash, read, write, edit, glob, grep
│   │   ├── client.h/cpp           # HTTPS + SSE 解析
│   │   ├── acp.h                  # ACP 协议
│   │   ├── acp_client.h/cpp       # ACP 客户端
│   │   └── log.h                  # 日志系统
│   └── util/src/config.h/cpp      # ProviderConfig + AppConfig
│
├── config/config.toml
├── bin/  (启动脚本)
└── scripts/build.sh
```

---

## Tool Registry

### 接口

```cpp
enum class Permission { Denied, Ask, Allow };

struct ToolSchema {
    std::string name, description;
    json parameters;  // JSON Schema
};

class Tool {
    virtual ToolSchema schema() const = 0;
    virtual Permission default_permission() const = 0;
    virtual ToolResult execute(const ToolCall&) = 0;
};

class ToolRegistry {
    void register_tool(unique_ptr<Tool>);
    vector<ToolSchema> all_schemas() const;
    ToolResult execute(const ToolCall&);
    Permission check_permission(name);
};
```

### 6 个内置工具

| Tool | Permission | 实现 |
|------|-----------|------|
| `bash` | Ask | fork+exec, 30s 超时, stdout/stderr 管道, 64KB 截断 |
| `read` | Allow | std::ifstream, offset/limit 分页, 行号前缀 |
| `write` | Allow | std::ofstream, 自动 mkdir |
| `edit` | Ask | find→replace→write, replaceAll, .bak 备份 |
| `glob` | Allow | recursive_directory_iterator, **/*.cpp 匹配 |
| `grep` | Allow | std::regex, 递归搜索, 扩展名过滤, 200 匹配上限 |

---

## ACP Loop

```
while (turn < 10):
    1. LLM stream (with tool schemas) → token frames
    2. extract_tool_calls(content)
       ├─ empty → break
       └─ has calls ↓
    3. for each tool_call:
       ├─ check_permission(name)
       │   ├─ Denied → error frame
       │   └─ Allow → execute()
       ├─ push tool_result frame
       └─ add to message history
    4. continue loop (LLM sees tool results)
```

---

## ACP 协议事件

| Event | SSE 帧 |
|-------|--------|
| `assistant` | `data: {"type":"assistant","data":{"delta":"..."}}` |
| `tool_call` | `data: {"type":"tool_call","data":{"id":"x","name":"bash","arguments":{...}}}` |
| `tool_result` | `data: {"type":"tool_result","data":{"id":"x","success":true,"content":"..."}}` |
| `error` | `data: {"type":"error","data":{"message":"..."}}` |
| `done` | `data: {"type":"done","data":{}}` |

---

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | 健康 + tools + providers |
| `GET` | `/api/v1/info` | 服务信息 |
| `POST` | `/api/v1/chat` | 非流式 (含 tools) |
| `POST` | `/api/v1/acp` | ACP SSE + 多轮 tool |
| `POST` | `/api/v1/sessions` | 创建会话 |
| `GET` | `/api/v1/sessions/:id` | 获取 |
| `POST` | `/api/v1/sessions/:id/messages` | 添加消息 |

---

## MVP 路线

| 阶段 | 版本 | 交付 |
|------|------|------|
| Phase 1 | v0.1.0 | CLI + 非流式 LLM + TOML |
| Phase 2 | v0.2.0 | C/S 架构 + REST API + Session |
| Phase 3 | v0.3.0 | ACP + SSE 实时流式 |
| Phase 3.1 | v0.3.1 | 多 Provider + 日志 + SSL 修复 |
| Phase 4 | v0.4.0 | **Tool Registry + 6 工具 + ACP 多轮 loop** |
| Phase 5 | v0.5.0 | TUI (FTXUI) + Anthropic |
| Phase 6 | v0.6.0 | SQLite 持久化 + System Context |
| Phase 7 | v0.7.0 | Plugin 系统 (C ABI) |
