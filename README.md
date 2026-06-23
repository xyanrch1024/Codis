# Codis — C++ AI Coding Agent

基于 C++20 的 AI 编程助手，Client/Server 架构，通过 ACP (Agent Communication Protocol) + SSE 实现实时流式对话。

## 架构

```
┌─────────────┐   ACP + SSE (HTTP)    ┌─────────────────────────┐
│  codis       │ ◄──────────────────► │  codis-server             │
│  (CLI/TUI)  │    text/event-stream  │  (后台守护进程)             │
│             │                       │                           │
│  AcpClient  │   POST /api/v1/acp    │  ├─ ProviderRegistry       │
│  事件回调     │   ─────────────────   │  │   ├─ OpenAI             │
│             │   ◄─ SSE frames ────  │  │   ├─ DeepSeek           │
│             │                       │  │   └─ Groq/...           │
└─────────────┘                       │  ├─ SessionManager         │
                                      │  └─ SseFrameQueue          │
                                      └─────────────────────────┘
                                               │
                                        ┌──────┴──────────────┐
                                        │  OpenAI / DeepSeek  │
                                        │  Groq / Anthropic   │
                                        └─────────────────────┘
```

## 特性

- **C/S 架构** — Server 守护进程 + 轻量 CLI 客户端
- **多 Provider** — OpenAI / DeepSeek / Groq / ...，配置文件驱动，零代码新增
- **ACP 协议** — SSE 实时流式推送，5 种事件类型
- **日志系统** — 5 级日志，环境变量控制，输出到 stderr + 文件
- **vcpkg 包管理** — 6 个第三方库，统一版本，零 FetchContent
- **C++20** — 现代化 C++，协程就绪

## 快速开始

### 前提条件

```bash
# 安装 vcpkg
git clone --depth 1 https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

# 设置环境变量
export VCPKG_ROOT=~/vcpkg
export OPENAI_API_KEY="sk-..."      # 或 DEEPSEEK_API_KEY
```

### 构建

```bash
git clone https://github.com/xyanrch1024/Codis.git
cd Codis

cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -- -j$(nproc)
```

### 运行

```bash
# 启动 Server（debug 日志）
OPENCODE_LOG_LEVEL=debug ./build/packages/server/opencode-server -p 8711

# 启动 Client
./build/packages/cli/opencode -i

# 单次调用
./build/packages/cli/opencode "你好，请介绍一下 C++20 协程"
```

### curl 直接调用

```bash
# 健康检查
curl http://localhost:8711/api/v1/health

# ACP SSE 流式对话
curl -N -X POST http://localhost:8711/api/v1/acp \
  -H "Content-Type: application/json" \
  -d '{"provider":"deepseek","model":"deepseek-chat","messages":[{"role":"user","content":"Hello"}]}'
```

## 配置

```toml
# config/config.toml
default_provider = "deepseek"

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

```bash
./build/packages/server/opencode-server -p 8711 -c config/config.toml
```

## 日志

```bash
# 级别: trace / debug / info / warn / error / off
OPENCODE_LOG_LEVEL=debug ./opencode-server -p 8711
OPENCODE_LOG_LEVEL=trace ./opencode-server -p 8711

# 输出到文件
OPENCODE_LOG_FILE=/tmp/codis.log OPENCODE_LOG_LEVEL=debug ./opencode-server -p 8711
```

输出格式：`[HH:MM:SS.ms] [LEVEL] [tID] file:line  msg`

## REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/v1/health` | 健康检查 + provider 列表 |
| `GET` | `/api/v1/info` | 服务信息 |
| `POST` | `/api/v1/chat` | 非流式聊天 |
| `POST` | `/api/v1/acp` | ACP SSE 流式对话 |
| `POST` | `/api/v1/sessions` | 创建会话 |
| `GET` | `/api/v1/sessions/:id` | 获取会话 |

## 项目结构

```
Codis/
├── CMakeLists.txt                 # vcpkg toolchain
├── vcpkg.json                     # 6 个依赖
├── ARCHITECTURE.md                # 架构设计文档
├── opencode-cpp-design.md         # 总体设计文档
│
├── packages/
│   ├── cli/                       # CLI 客户端
│   ├── server/                    # HTTP 守护进程
│   ├── llm/                       # 共享库 (Provider + ACP + Log)
│   └── util/                      # 配置
│
├── config/config.toml             # 示例配置
└── scripts/build.sh               # 构建脚本
```

## 技术栈

| 库 | 用途 | 版本 |
|----|------|------|
| cpp-httplib | HTTP 客户端/服务端 | 0.47.0 |
| nlohmann/json | JSON | 3.12.0 |
| CLI11 | CLI 参数 | 2.6.2 |
| toml++ | 配置文件 | 3.4.0 |
| OpenSSL | SSL/TLS | 3.6.3 |
| standalone asio | 异步 IO + 信号 | 1.32.0 |

## License

MIT
