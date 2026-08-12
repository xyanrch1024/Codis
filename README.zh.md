# Codis — C++ AI 编程助手

基于 C++20 的 AI 编程助手：多 Provider、多客户端共享 Session、飞书 Bot、终端 TUI，一套服务端 + 多种客户端。

> 🇺🇸 [English README](./README.md)

![Codis](docs/codis.png)

## 特点

- **多 Provider** — OpenAI / DeepSeek / GLM / Groq，配置文件驱动，可随时切换
- **全双工 WebSocket** — 一条连接既发请求又收流式推送，内容按 token 实时输出
- **多客户端共享会话** — CLI / TUI / 飞书 Bot 共用同一 session，互不干扰
- **消息不丢失** — 处理中到达的消息排队补跑；断线期间发送的请求重连后自动补发
- **实时流式输出** — 模型回复按字/按 token 实时显示（含思维链，默认不折叠）
- **工具调用** — bash / read / write / edit / glob / grep，支持 C ABI 插件动态扩展
- **会话持久化** — SQLite 存储，支持恢复历史、切换、删除、搜索
- **终端 TUI** — FTXUI 界面：颜色区分消息、滚轮滚动、双击 ESC 取消当前任务
- **飞书 Bot** — WebSocket 长连接，无需公网 IP
- **Docker 一键部署** — 单容器运行

## 编译

依赖：CMake 3.20+、vcpkg、C++20 编译器。

```bash
# 1. 安装依赖（vcpkg 已在系统上时跳过）
#    vcpkg install cpp-httplib nlohmann-json cli11 tomlplusplus openssl sqlite3 ftxui asio

# 2. 配置 + 编译
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j$(nproc)
```

## 运行

```bash
# 终端 1: 启动服务端
export GLM_API_KEY="你的-api-key"
./build/packages/server/opencode-server -c config/config.toml

# 终端 2: 交互式 CLI（默认）
./build/packages/cli/opencode

# 启动 TUI（推荐）
./build/packages/cli/opencode --tui

# 继续上次会话
./build/packages/cli/opencode --tui -c

# 指定端口 / 指定 provider
./build/packages/cli/opencode --tui -p 8711 -m glm-4.5-flash
```

也可以不用手动启动服务端：CLI 检测到服务端未运行时会自动拉起。

### 常用快捷键（TUI）

| 按键 | 功能 |
|------|------|
| `↑` `↓` / 鼠标滚轮 | 滚动对话区 |
| 双击 `ESC` | 取消当前正在执行的任务 |
| `Ctrl+S` | 打开会话列表 |
| `Ctrl+C` | 退出 |

### 常用命令

| 命令 | 功能 |
|------|------|
| `/sessions` | 列出所有会话 |
| `/session <id> use` | 恢复指定会话 |
| `/session <id> del` | 删除指定会话 |
| `/newsession` | 新建会话 |
| `/clear` | 清空当前上下文 |
| `/clearsessions` | 删除所有会话 |
| `/balance [provider]` | 查询 Provider 余额 |
| `/exit` | 退出 |

### 日志

| 环境变量 | 默认值 | 说明 |
|------|------|------|
| `OPENCODE_LOG_LEVEL` | `info` | `trace` / `debug` / `info` / `warn` / `error` / `off` |
| `OPENCODE_LOG_FILE` | 未设 | 设置后日志只写文件（保持全屏 TUI 干净）；否则输出到 stderr |

## 配置

```toml
default_provider = "glm"

[llm]
max_tokens = 4096
temperature = 0.7

[[providers]]
name = "glm"
api_key_env = "GLM_API_KEY"
model = "glm-4.5-flash"
base_url = "https://open.bigmodel.cn/api/paas/v4"
```

API Key 通过环境变量设置，不要在配置文件中写明文。

## Docker

```bash
docker build -t codis .
docker run -d --name codis \
  -e GLM_API_KEY="xxx" \
  -e FEISHU_APP_ID="cli_xxx" \
  -e FEISHU_APP_SECRET="xxx" \
  -p 8711:8711 \
  codis
docker logs -f codis
```

## 技术栈

| 模块 | 库 |
|------|-----|
| HTTP / WebSocket | cpp-httplib |
| JSON | nlohmann/json |
| CLI 参数 | CLI11 |
| 配置 | toml++ |
| SSL | OpenSSL |
| 异步 IO | standalone asio |
| 数据库 | SQLite3 |
| TUI | FTXUI |
| 飞书 SDK | lark-oapi (Python) |
| 构建 | CMake / vcpkg |

## 项目结构

```
packages/
├── server/   # 服务端守护进程（HTTP + WebSocket + LLM 调度 + 工具执行）
└── cli/      # 客户端（交互 CLI + FTXUI TUI）
└── llm/      # Provider 封装 / 会话存储 / 工具注册（server 与 cli 共用）
```
