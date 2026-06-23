# 基于 C++20 的 OpenCode AI Coding Agent 架构设计

## 项目状态

| 阶段 | 版本 | 关键交付 | 状态 |
|------|------|----------|------|
| Phase 1 | v0.1.0 | 单体 CLI + 非流式 LLM + TOML | 完成 |
| Phase 2 | v0.2.0 | C/S REST + Session | 完成 |
| Phase 3 | v0.3.0 | ACP + SSE 实时流式 + 事件回调 | 完成 |
| Phase 4 | v0.3.1 | **多 Provider** + **日志系统** + **SSL 修复** | 完成 |
| Phase 5 | v0.4.0 | Tool Registry + 工具执行 | 规划中 |

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
| C++ 标准 | **C++20** | | |
| 构建系统 | **CMake** 3.20+ | | |
| 包管理 | **vcpkg** manifest (6 包) | | |
| 通信协议 | **ACP** over HTTP SSE | | |

---

## 项目目录

```
opencode-cpp/
├── CMakeLists.txt                 # 6 个 find_package
├── vcpkg.json                     # asio, httplib[openssl], nlohmann-json, CLI11, toml++, openssl
├── ARCHITECTURE.md                # C/S 架构详解
├── opencode-cpp-design.md         # 本文档
│
├── packages/
│   ├── cli/src/main.cpp           # CLI → AcpClient → 事件回调 + log
│   ├── server/src/                # HTTP 守护进程 (多 provider)
│   │   ├── main.cpp               # -p port -c config + log
│   │   ├── server.h               # OpenCodeServer + ProviderRegistry + SseFrameQueue
│   │   └── server.cpp             # 7 REST 端点 + resolve_provider()
│   ├── llm/src/
│   │   ├── types.h                # Message, ChatRequest, ChatResponse
│   │   ├── provider.h             # LLMProvider 抽象
│   │   ├── openai_compatible_provider.h/cpp  # 通用兼容层 (OpenAI/DeepSeek/Groq/...)
│   │   ├── provider_registry.h    # 多 provider 注册表
│   │   ├── client.h/cpp           # HTTPS 客户端 + SSE 解析
│   │   ├── acp.h                  # ACP 协议
│   │   ├── acp_client.h/cpp       # ACP 客户端
│   │   └── log.h                  # 日志系统 (5 级)
│   └── util/src/config.h/cpp      # 配置 (api_key_env)
│
├── config/config.toml
├── bin/  (启动脚本)
└── scripts/build.sh
```

---

## 核心模块

### 1. OpenAICompatibleProvider — 通用 LLM Provider

```cpp
// 一份代码，覆盖所有 OpenAI 兼容 API
// 新增 service 只需配置，零 C++ 代码

class OpenAICompatibleProvider : public LLMProvider {
    ProviderConfig cfg;  // name, api_key, model, base_url

    ChatResponse chat(const ChatRequest&);        // POST /chat/completions
    ChatResponse stream_chat(const ChatRequest&,  // POST + SSE 解析
                              TokenCallback);
};
```

### 2. LLMHttpClient — HTTPS + SSE

```cpp
// 底层 HTTP 传输
// 非流式: POST → 解析 JSON response
// 流式:   POST stream=true → 逐行 SSE 解析 → on_token

class LLMHttpClient {
    void stream_post(url, api_key, body,
                     on_token, on_done,
                     timeout, non_stream);
};
```

### 3. ProviderRegistry — 多 Provider 管理

```cpp
class ProviderRegistry {
    void register_provider(ProviderConfig);     // 注册
    optional<shared_ptr<LLMProvider>> get(name);  // 按名获取
    vector<string> list();                       // 列出所有
};
```

### 4. SseFrameQueue — Producer-Consumer

```cpp
// 桥接 LLM 异步流与 HTTP chunked 输出
class SseFrameQueue {
    void push(frame);   // LLM 线程写入
    string pop();       // HTTP 线程读出 (阻塞)
};
```

### 5. ACP 协议层

```cpp
namespace acp {
    enum class EventType { assistant, tool_call, tool_result, error, done };

    string assistant_frame(delta);          // SSE 帧序列化
    string done_frame();
    optional<ParsedEvent> parse_frame(line); // SSE 帧解析
}
```

### 6. 日志系统 (log.h)

```cpp
// header-only, 5 级, 环境变量控制, 线程安全
LOG_TRACE("detail: {}", value);
LOG_DEBUG("POST {} bytes", n);
LOG_INFO("server listening on port {}", port);
LOG_WARN("retry {}/{}", n, max);
LOG_ERROR("connection failed: {}", msg);

// Output: [14:32:05.123] [DEBUG] [t3] client.cpp:53  POST https://...
```

---

## 依赖关系图

```
                          vcpkg (6 包)
                   ┌─────────────────────────┐
                   │ asio                    │
                   │ cpp-httplib [openssl]   │
                   │ nlohmann-json           │
                   │ CLI11                   │
                   │ toml++                  │
                   │ OpenSSL                 │
                   └──────────┬──────────────┘
                              │
                        CMake 构建
                              │
         ┌────────────────────┼──────────────────┐
         │                    │                  │
   opencode             opencode-server     libopencode_*
   (CLI)                 (HTTP daemon)       (共享库)
         │                    │                  │
   AcpClient ──ACP SSE──► handle_acp()      LLMProvider
   事件回调                SseFrameQueue      Client
         │                    │                  │
      log.h                 log.h             log.h
```

---

## 端到端数据流 (SSE 流式)

```
1. Client:  POST /api/v1/acp  ChatRequest(JSON)
2. Server:  handle_acp() → detach LLM worker
3. Worker:  resolve_provider() → stream_chat()
4. Provider: stream_post() → POST https://api.deepseek.com/...
5. HTTP:    获取完整 SSE body
6. Parser:  逐行解析 "data: {...}" → on_token(delta)
7. Provider: → SseFrameQueue::push(assistant_frame(delta))
8. Server:   chunked_content_provider → pop() → sink.write()
9. Client:   接收 text/event-stream → parse_sse() → on_assistant(delta)
```

---

## 构建与运行

```bash
# 构建
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build

# 运行
export DEEPSEEK_API_KEY="sk-..."
OPENCODE_LOG_LEVEL=debug ./build/packages/server/opencode-server -p 8711 -c config/config.toml
./build/packages/cli/opencode -i
```

---

## 已修复问题 (v0.3.1)

| 问题 | 根因 | 修复 |
|------|------|------|
| CLI 无响应 / SSE 流式不工作 | `LLMHttpClient` 流式路径是 TODO 空壳 | 实现 SSE 逐行解析 `choices[0].delta.content` |
| HTTPS 返回 302 | `httplib::Client(host)` 不带 scheme | 改为 `Client("https://"+host)` |
| `https` scheme 不支持 | vcpkg cpp-httplib 默认无 OpenSSL | `vcpkg.json`: `httplib[openssl]` |

---

## MVP 路线

| 阶段 | 版本 | 交付 |
|------|------|------|
| Phase 1 | v0.1.0 | CLI + 非流式 LLM + TOML 配置 |
| Phase 2 | v0.2.0 | C/S 架构 + REST API + Session |
| Phase 3 | v0.3.0 | ACP + SSE 实时流式 + 事件回调 |
| Phase 4 | v0.3.1 | **多 Provider** + **日志系统** + **SSL 修复** |
| Phase 5 | v0.4.0 | Tool Registry + Bash/Read/Write/Edit |
| Phase 6 | v0.5.0 | TUI (FTXUI) + Anthropic Provider |
| Phase 7 | v0.6.0 | SQLite 持久化 + System Context |
| Phase 8 | v0.7.0 | Plugin 系统 (C ABI) |
