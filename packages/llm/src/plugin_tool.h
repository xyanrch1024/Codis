#pragma once

#include "tool.h"
#include "plugin.h"

#include <string>

namespace opencode {

class PluginTool : public Tool {
public:
    PluginTool(std::string name, std::string desc, json params,
               codis_tool_execute_fn execute, void* ctx)
        : name_(std::move(name))
        , description_(std::move(desc))
        , params_(std::move(params))
        , execute_fn_(execute)
        , ctx_(ctx)
    {}

    ToolSchema schema() const override {
        ToolSchema s;
        s.name = name_;
        s.description = description_;
        s.parameters = params_;
        return s;
    }

    Permission default_permission() const override { return Permission::Ask; }

    ToolResult execute(const ToolCall& call) override {
        if (!execute_fn_) return {call.id, false, "plugin tool execute_fn is null"};
        std::string args_json = call.arguments.dump();
        char* raw = execute_fn_(args_json.c_str(), ctx_);
        if (!raw) return {call.id, false, "plugin returned null"};
        std::string result(raw);
        free(raw);
        return {call.id, true, result};
    }

private:
    std::string name_;
    std::string description_;
    json params_;
    codis_tool_execute_fn execute_fn_;
    void* ctx_;
};

} // namespace opencode
