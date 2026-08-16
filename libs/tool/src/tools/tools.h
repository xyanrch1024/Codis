#pragma once

#include "tool.h"
#include <string>
#include <utility>

namespace codis::tools {

class BashTool : public Tool {
public:
    ToolSchema schema() const override;
    Permission default_permission() const override { return Permission::Allow; }
    ToolResult execute(const ToolCall& call) override;
};

class ReadTool : public Tool {
public:
    ToolSchema schema() const override;
    Permission default_permission() const override { return Permission::Allow; }
    ToolResult execute(const ToolCall& call) override;
};

class WriteTool : public Tool {
public:
    ToolSchema schema() const override;
    Permission default_permission() const override { return Permission::Ask; }
    ToolResult execute(const ToolCall& call) override;
};

class EditTool : public Tool {
public:
    ToolSchema schema() const override;
    Permission default_permission() const override { return Permission::Ask; }
    ToolResult execute(const ToolCall& call) override;
};

class GlobTool : public Tool {
public:
    ToolSchema schema() const override;
    Permission default_permission() const override { return Permission::Allow; }
    ToolResult execute(const ToolCall& call) override;
};

class GrepTool : public Tool {
public:
    ToolSchema schema() const override;
    Permission default_permission() const override { return Permission::Allow; }
    ToolResult execute(const ToolCall& call) override;
};

// WebSearch 工具运行配置（由 server 从 config [websearch] 注入，工具库不依赖 AppConfig）
struct WebSearchOptions {
    std::string backend = "bing";   // bing | serpapi | brave | tavily
    std::string api_key;
    int max_results = 5;
    int timeout_seconds = 15;
};

class WebSearchTool : public Tool {
public:
    explicit WebSearchTool(WebSearchOptions opts = {}) : opts_(std::move(opts)) {}
    ToolSchema schema() const override;
    Permission default_permission() const override { return Permission::Allow; }
    ToolResult execute(const ToolCall& call) override;
    const WebSearchOptions& options() const { return opts_; }

private:
    WebSearchOptions opts_;
};

} // namespace codis::tools
