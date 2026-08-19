# Codis is an AI code assistant written in C++.

A high-performance AI Coding Agent framework written in modern C++20. Designed with a Client/Server architecture, it supports multiple AI providers and shared multi-client sessions, delivering low-latency, scalable AI assistance for code generation and refactoring.

> 🇨🇳 [中文 README](./README.zh.md)

![Codis](docs/codis.png)

## Features

- **Multi-Provider** — OpenAI / DeepSeek / GLM / Groq, config-driven, switchable at runtime
- **Full-duplex WebSocket** — one connection for both requests and streamed responses; tokens arrive in real time
- **Real-time streaming** — assistant output (including reasoning) streams token-by-token
- **Tool calls** — bash / read / write / edit / glob / grep, plus C-ABI plugins for custom tools
- **Session persistence** — SQLite-backed history with restore / switch / delete / search
- **Terminal TUI** — FTXUI: color-coded messages, mouse-wheel scrolling, double-ESC cancels the running task
- **HTTP proxy support** — global proxy in `config.toml` for all outbound HTTP (LLM / websearch / MCP), per-provider override supported

## Build

Requirements: CMake 3.20+, vcpkg, a C++20 compiler (GCC 12+ / Clang 15+), Ninja.

Dependencies are managed by vcpkg in manifest mode — everything in `vcpkg.json` is installed automatically during the configure step.

### Linux

```bash
# 1. Install the base toolchain (Debian/Ubuntu)
sudo apt-get update -y
sudo apt-get install -y cmake ninja-build g++ gcc make pkg-config git curl zip unzip tar ca-certificates

# 2. Install and bootstrap vcpkg (clone into a local directory)
git clone https://github.com/microsoft/vcpkg.git ./vcpkg
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

# 3. Configure (installs all third-party deps via vcpkg manifest, ~4 min first time)
export VCPKG_ROOT=$(pwd)/vcpkg
cmake -B build -S . -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON

# 4. Build
cmake --build build -j$(nproc)
```

Binaries are output to `build/bin/` (`codis`, `codis-server`).

### Run tests (optional)

```bash
cd build && ctest --output-on-failure
```

### Install (optional)

```bash
cmake --build build --target install
```

Install rules: `bin/` (executables), `etc/codis/config.toml` (config), `lib/codis/plugins/*.so` (plugins).


## Run

```bash
# Terminal 1: start the server
export GLM_API_KEY="your-api-key"
./build/bin/codis-server -c config/config.toml

# Terminal 2: launch the TUI client (default)
./build/bin/codis

# Continue the last session
./build/bin/codis -c

# Custom port / model
./build/bin/codis -p 8711 -m glm-4.5-flash
```

No need to start the server manually — the CLI auto-starts it if it isn't running.

### Commands

| Command | Description |
|------|------|
| `/help` | Show the command list |
| `/exit` | Quit |
| `/clear` | Clear the current context |
| `/sessions` | List all sessions (or `Ctrl+S` for the session list overlay) |
| `/newsession` | Create a new session |
| `/balance [provider]` | Query provider balance |
| `/model [provider]` | Open model dropdown picker (Tab/↑↓ to select, Enter to apply); `/model <provider>` switches directly |
| `/clearsessions` | Delete all sessions |
| `/yolo [on\|off]` | Toggle YOLO mode — auto-approve all Ask tools (no confirmation prompts) |
| `/compact` | Compress the context (LLM summary of the history) |
| `/info` | Show skills & MCP servers |

### Logging

| Env | Default | Description |
|------|------|------|
| `CODIS_LOG_LEVEL` | `info` | `trace` / `debug` / `info` / `warn` / `error` / `off` |
| `CODIS_LOG_FILE` | unset | When set, logs go to the file only (keeps the fullscreen TUI clean); otherwise to stderr |

## Configuration

```toml
default_provider = "glm"

# Optional global HTTP proxy "host:port". All outbound HTTP (LLM provider /
# websearch / MCP http) uses it by default; a per-provider `proxy` overrides
# it. Leave empty for direct connection.
proxy = "127.0.0.1:7890"

[llm]
max_tokens = 4096
temperature = 0.7

[[providers]]
name = "glm"
api_key_env = "GLM_API_KEY"
model = "glm-4.5-flash"
base_url = "https://open.bigmodel.cn/api/paas/v4"
# proxy = "127.0.0.1:7890"   # optional: override the global proxy for this provider
```

API keys are set via environment variables — never in the config file.

## Project Layout

```
libs/
├── server/   # Server daemon (HTTP + WebSocket + LLM scheduling + tool execution)
├── tui/      # FTXUI TUI client (model/views/controller/tui composition)
└── llm/      # Provider wrappers / session storage / tool registry (shared)
```
