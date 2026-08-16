#pragma once

#include "mcp_client.h"
#include "tool.h"

#include <string>

namespace codis::mcp {

// MCP 工具适配层：包装一个远端工具为本地 Tool（schema 透传，执行转发 tools/call）
class McpTool : public Tool {
public:
    McpTool(std::string reg_name, McpClient* client, McpToolInfo info)
        : reg_name_(std::move(reg_name)), client_(client), info_(std::move(info)) {}

    ToolSchema schema() const override {
        ToolSchema s;
        s.name = reg_name_;
        s.description = info_.description.empty()
                            ? std::string("MCP tool exported by server '") + client_->name() + "'"
                            : info_.description;
        s.parameters = info_.input_schema;
        return s;
    }

    // 外部副作用：默认需要确认（与 PluginTool 同级），可用 [permissions] 覆写
    Permission default_permission() const override { return Permission::Ask; }

    ToolResult execute(const ToolCall& call) override {
        json args = call.arguments.is_object() ? call.arguments : json::object();
        try {
            McpCallResult r = client_->call_tool(info_.name, args);
            if (!r.structured.is_null() && r.text.empty())
                r.text = r.structured.dump();
            return {call.id, !r.error, r.text};
        } catch (const std::exception& e) {
            return {call.id, false, std::string("MCP call failed: ") + e.what()};
        }
    }

    // 远端原始工具名（供管理/日志）
    const std::string& remote_name() const { return info_.name; }

private:
    std::string reg_name_;
    McpClient* client_;
    McpToolInfo info_;
};

} // namespace codis::mcp