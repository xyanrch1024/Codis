#pragma once

#include "tool.h"
#include <string>

namespace opencode::tools {

class BashTool : public Tool {
public:
    ToolSchema schema() const override;
    Permission default_permission() const override { return Permission::Ask; }
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

} // namespace opencode::tools
