---
name: Codis C++ 编码约定
description: 本仓库的 C++ 编码规范（命名/注释/依赖/风格/提交）。写代码或 review 前加载。
---

# Codis C++ 编码约定

遵循本仓库既有风格，与现有代码保持一致比任何抽象规范优先。

## 通用
- 注释用中文；只解释"为什么"，不写"是什么"（代码本身表达 what）
- 头文件 `#pragma once`
- 函数/变量 snake_case，类型/类 PascalCase，常量 kCamelCase
- 缩进 4 空格，无制表符；行宽约 100
- 不使用 `using namespace std`

## 库与结构
- 各功能在 `libs/<name>/src` 下成库（cli / tool / server / llm / util / protocol），库内头文件放 `libs/<name>/include` 或同目录，CMake 用 `target_include_directories`
- header-only 小型工具放 `libs/tui/src/*.h`（inline 函数，如 md_render.h）
- 依赖库：nlohmann_json、httplib、ftxui、toml++、spdlog；新增第三方依赖需用 vcpkg manifest

## 工具（tool）开发
- 新工具继承 `codis::Tool`（tool.h）：`schema()`（OpenAI JSON Schema）、`default_permission()`、`execute()`
- 工具库不依赖 `AppConfig`：运行期参数用 Options 结构体由 server 注入（参考 `WebSearchOptions`）
- 执行结果统一经 `ToolRegistry::execute` 清理 UTF-8 后进模型上下文

## 渲染与 TUI
- `md_render.h` 是自研轻量 markdown 渲染器：块级标题/围栏/表格/列表/引用/分隔线/段落 + 行内粗斜体/代码/链接
- 文本宽度一律按**显示宽度**处理（CJK 等宽字符 2 列），用 `string_width` / `wrap_by_width`，不要按字节数对齐

## 提交
- 提交信息中文、`<模块>: <改动摘要>` 格式（如 `md_render: 新增 GFM 表格渲染`）
- 配置/密钥不入库（start.sh 已 gitignore）；API key 走 `api_key_env` 环境变量