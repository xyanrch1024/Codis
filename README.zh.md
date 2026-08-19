# Codis — 用 C++ 编写的 AI 代码助手

用现代 C++20 编写的高性能 AI 编码代理框架。采用客户端/服务器架构，支持多种 AI Provider 与多客户端共享会话，为代码生成与重构提供低延迟、可扩展的 AI 辅助。

> 🇺🇸 [English README](./README.md)

![Codis](docs/codis.png)

## 特点

- **多 Provider** — OpenAI / DeepSeek / GLM / Groq，配置文件驱动，可随时切换
- **全双工 WebSocket** — 一条连接既发请求又收流式推送，内容按 token 实时输出
- **实时流式输出** — 模型回复按字/按 token 实时显示（含思维链，默认不折叠）
- **工具调用** — bash / read / write / edit / glob / grep，支持 C ABI 插件动态扩展
- **会话持久化** — SQLite 存储，支持恢复历史、切换、删除、搜索
- **终端 TUI** — FTXUI 界面：颜色区分消息、滚轮滚动、双击 ESC 取消当前任务
- **HTTP 代理支持** — `config.toml` 顶层 proxy 统一走代理（LLM / websearch / MCP），provider 可单独覆盖

## 编译

依赖：CMake 3.20+、vcpkg、C++20 编译器（GCC 12+ / Clang 15+）、Ninja。

第三方依赖由 vcpkg manifest 模式管理，`vcpkg.json` 中的全部依赖会在配置阶段自动安装。

### Linux

```bash
# 1. 安装基础工具链（Debian/Ubuntu）
sudo apt-get update -y
sudo apt-get install -y cmake ninja-build g++ gcc make pkg-config git curl zip unzip tar ca-certificates

# 2. 安装并 bootstrap vcpkg（克隆到本地目录）
git clone https://github.com/microsoft/vcpkg.git ./vcpkg
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

# 3. 配置（自动通过 vcpkg manifest 安装全部第三方依赖，首次约 4 分钟）
export VCPKG_ROOT=$(pwd)/vcpkg
cmake -B build -S . -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON

# 4. 编译
cmake --build build -j$(nproc)
```

编译产物输出到 `build/bin/`（`codis`、`codis-server`）。

### 运行测试（可选）

```bash
cd build && ctest --output-on-failure
```

### 安装（可选）

```bash
cmake --build build --target install
```

安装规则：`bin/`（可执行文件）、`etc/codis/config.toml`（配置）、`lib/codis/plugins/*.so`（插件）。


## 运行

```bash
# 终端 1: 启动服务端
export GLM_API_KEY="你的-api-key"
./build/bin/codis-server -c config/config.toml

# 终端 2: 启动 TUI 客户端（默认）
./build/bin/codis

# 继续上次会话
./build/bin/codis -c

# 指定端口 / 指定模型
./build/bin/codis -p 8711 -m glm-4.5-flash
```

也可以不用手动启动服务端：CLI 检测到服务端未运行时会自动拉起。

### 常用命令

| 命令 | 功能 |
|------|------|
| `/help` | 显示命令帮助 |
| `/exit` | 退出 |
| `/clear` | 清空当前上下文 |
| `/sessions` | 列出所有会话（或 `Ctrl+S` 打开会话列表弹窗） |
| `/newsession` | 新建会话 |
| `/balance [provider]` | 查询 provider 余额 |
| `/model [provider]` | 弹出模型下拉选择面板（Tab/↑↓ 选择，Enter 应用）；`/model <provider>` 直接切换 |
| `/clearsessions` | 删除所有会话 |
| `/yolo [on\|off]` | 切换 YOLO 模式 — 所有 Ask 工具自动批准（不再弹确认） |
| `/compact` | 压缩上下文（LLM 摘要） |
| `/info` | 查看技能（skills）与 MCP 服务器 |

### 日志

| 环境变量 | 默认值 | 说明 |
|------|------|------|
| `CODIS_LOG_LEVEL` | `info` | `trace` / `debug` / `info` / `warn` / `error` / `off` |
| `CODIS_LOG_FILE` | 未设 | 设置后日志只写文件（保持全屏 TUI 干净）；否则输出到 stderr |

## 配置

```toml
default_provider = "glm"

# 可选：全局 HTTP 代理 "host:port"。所有出站 HTTP（LLM provider / websearch /
# MCP http）默认走该代理；[[providers]] 内可单独写 proxy 覆盖，留空则直连。
proxy = "127.0.0.1:7890"

[llm]
max_tokens = 4096
temperature = 0.7

[[providers]]
name = "glm"
api_key_env = "GLM_API_KEY"
model = "glm-4.5-flash"
base_url = "https://open.bigmodel.cn/api/paas/v4"
# proxy = "127.0.0.1:7890"   # 可选：为当前 provider 单独覆盖全局代理
```

API Key 通过环境变量设置，不要在配置文件中写明文。

## 项目结构

```
libs/
├── server/   # 服务端守护进程（HTTP + WebSocket + LLM 调度 + 工具执行）
├── tui/      # FTXUI TUI 客户端（model / views / controller / tui 四层）
└── llm/      # Provider 封装 / 会话存储 / 工具注册（server 与 tui 共用）
```
